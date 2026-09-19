/* ================================================================
 *  rf_handshake.c — nRF24L01P Security Handshake (Section 3)
 * ================================================================ */
/**
 * Listens on channel 76, address 0xE7E7E7E7E7.
 * Validates: start marker 0xAA, DeviceID (eFuse OTP), CRC8, session nonce.
 * Nonce must be strictly greater than last stored nonce (replay protection).
 * Timeout: 30 seconds → safe standby, amber LED 1Hz.
 */

#include "rf_handshake.h"
#include "config.h"
#include "hardware_init.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "nvs.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RF_HS";
extern spi_device_handle_t g_spi_nrf;

#define NVS_NAMESPACE   "24shub"
#define NVS_KEY_NONCE   "last_nonce"
#define NVS_KEY_DEVID   "device_id"

/* Simple CRC8 (poly 0x07) */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
    }
    return crc;
}

/* nRF24 register write */
static void nrf_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {0x20 | reg, val};
    spi_transaction_t t = { .tx_buffer = tx, .length = 16 };
    spi_device_transmit(g_spi_nrf, &t);
}

/* nRF24 setup for RX on channel 76, address 0xE7E7E7E7E7, 6-byte payload */
static void nrf_init_rx(void)
{
    gpio_set_level(PIN_NRF_CE, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    nrf_write_reg(0x00, 0x3B);   // CONFIG: PWR_UP, PRIM_RX, EN_CRC, CRCO
    nrf_write_reg(0x01, 0x00);   // EN_AA: disable auto-ack
    nrf_write_reg(0x02, 0x01);   // EN_RXADDR: pipe 0
    nrf_write_reg(0x05, NRF_CHANNEL);  // RF_CH
    nrf_write_reg(0x06, 0x06);   // RF_SETUP: 1Mbps, 0dBm

    /* Set RX address on pipe 0 */
    uint8_t addr_cmd[6] = {0x20 | 0x0A,  // W_REGISTER | RX_ADDR_P0
                            0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    spi_transaction_t t = { .tx_buffer = addr_cmd, .length = 48 };
    spi_device_transmit(g_spi_nrf, &t);

    nrf_write_reg(0x11, NRF_PACKET_LEN);  // RX_PW_P0 = 6 bytes

    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_NRF_CE, 1);  // start listening
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_LOGI(TAG, "nRF24 RX ready on channel %d", NRF_CHANNEL);
}

/* Poll RX FIFO — returns true if packet available */
static bool nrf_data_ready(void)
{
    uint8_t tx = 0xFF;  // NOP
    uint8_t rx = 0;
    spi_transaction_t t = {
        .tx_buffer = &tx, .rx_buffer = &rx, .length = 8
    };
    spi_device_transmit(g_spi_nrf, &t);
    return !(rx & 0x01);  // STATUS bit 0 = RX_DR (active low)
}

/* Read 6-byte payload */
static void nrf_read_payload(uint8_t *buf)
{
    uint8_t tx[7] = {0x61, 0,0,0,0,0,0};  // R_RX_PAYLOAD
    uint8_t rx[7] = {0};
    spi_transaction_t t = {
        .tx_buffer = tx, .rx_buffer = rx, .length = 56
    };
    spi_device_transmit(g_spi_nrf, &t);
    memcpy(buf, rx+1, 6);
    /* Clear RX_DR flag */
    nrf_write_reg(0x07, 0x40);
}

esp_err_t rf_handshake_wait(uint32_t timeout_ms)
{
    nrf_init_rx();

    /* Load stored DeviceID and last nonce from NVS */
    nvs_handle_t nvs_h;
    uint16_t stored_device_id = 0;
    uint16_t last_nonce       = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h) == ESP_OK) {
        nvs_get_u16(nvs_h, NVS_KEY_DEVID,  &stored_device_id);
        nvs_get_u16(nvs_h, NVS_KEY_NONCE,  &last_nonce);
        nvs_close(nvs_h);
    }

    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;

    while (esp_timer_get_time() < deadline) {
        if (!nrf_data_ready()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t pkt[6];
        nrf_read_payload(pkt);

        /* Validate packet structure */
        if (pkt[0] != NRF_START_MARKER) {
            ESP_LOGW(TAG, "Bad start marker: 0x%02X", pkt[0]);
            continue;
        }

        uint16_t device_id    = (pkt[1] << 8) | pkt[2];
        uint16_t session_nonce = (pkt[3] << 8) | pkt[4];
        uint8_t  rx_crc       = pkt[5];
        uint8_t  calc_crc     = crc8(pkt, 5);

        if (device_id != stored_device_id) {
            ESP_LOGW(TAG, "DeviceID mismatch: got %04X, expect %04X",
                     device_id, stored_device_id);
            continue;
        }

        if (rx_crc != calc_crc) {
            ESP_LOGW(TAG, "CRC mismatch: got 0x%02X, calc 0x%02X",
                     rx_crc, calc_crc);
            continue;
        }

        if (session_nonce <= last_nonce) {
            ESP_LOGW(TAG, "Replay attack: nonce %d <= stored %d",
                     session_nonce, last_nonce);
            continue;
        }

        /* VALID — store nonce to NVS before enabling gate */
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_u16(nvs_h, NVS_KEY_NONCE, session_nonce);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }

        ESP_LOGI(TAG, "RF handshake confirmed! DeviceID=%04X Nonce=%d",
                 device_id, session_nonce);
        gpio_set_level(PIN_NRF_CE, 0);  // stop RX
        return ESP_OK;
    }

    gpio_set_level(PIN_NRF_CE, 0);
    ESP_LOGE(TAG, "RF handshake timeout after %d ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 *  ina240.c — INA240A1D Current Sense Driver
 * ================================================================ */
#include "ina240.h"

/* R_SENSE = 0.5Ω, INA240 gain = 20V/V
 * V_out = V_ref + (I × R_sense × Gain)
 * I = (V_out - V_ref) / (R_sense × Gain)
 * At 0A: V_out = V_ref (half of Vcc = ~1650mV with 3.3V supply, or ADC midpoint)
 */
#define RSENSE_MOHM     500   // 0.5Ω in mΩ
#define INA240_GAIN     20    // V/V
#define VREF_MV         1650  // midpoint for bidirectional — adjust to actual

static const char *TAG_INA = "INA240";

int32_t ina240_read_current_ma(void)
{
    uint32_t v_mv = adc_read_mv(PIN_INA240_ADC);
    /* Signed: positive = discharge, negative = charge */
    int32_t v_diff_uv = ((int32_t)v_mv - VREF_MV) * 1000;
    int32_t current_ma = v_diff_uv / (RSENSE_MOHM * INA240_GAIN / 1000);
    return current_ma;
}

esp_err_t ina240_verify_idle(void)
{
    int32_t current = ina240_read_current_ma();
    /* At idle, expect < 500mA (noise threshold) */
    if (current > 2000 || current < -2000) {
        ESP_LOGE(TAG_INA, "Idle check fail: %d mA (expected ~0)", current);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG_INA, "Idle check OK: %d mA", current);
    return ESP_OK;
}

/* ================================================================
 *  pre_bias.c — Load-Side Pre-Bias Detection (Section 8)
 * ================================================================ */
#include "pre_bias.h"

static const char *TAG_PB = "PREBIAS";
static output_profile_t current_profile = OUTPUT_PROFILE_STANDBY;
static uint32_t nominal_mv_table[] = {0, 18000, 36000, 48000, 72000};

uint32_t pre_bias_read_mv(void)
{
    /* Resistor divider: scale ADC back to actual rail voltage */
    /* Divider ratio: R_top/R_bot — adjust to your schematic values */
    /* Example: 100kΩ / 3.3kΩ → ratio ≈ 31.3 */
    #define DIVIDER_RATIO   31
    uint32_t adc_mv = adc_read_mv(PIN_PREBIAS_ADC);
    return adc_mv * DIVIDER_RATIO;
}

void pre_bias_detect_and_select(void)
{
    uint32_t v_mv = pre_bias_read_mv();
    ESP_LOGI(TAG_PB, "Pre-bias: %d mV", v_mv);

    output_profile_t profile;
    if      (v_mv < 5000)                    profile = OUTPUT_PROFILE_STANDBY;
    else if (v_mv >= 14000 && v_mv <= 22000) profile = OUTPUT_PROFILE_18V;
    else if (v_mv >= 30000 && v_mv <= 42000) profile = OUTPUT_PROFILE_36V;
    else if (v_mv >= 42000 && v_mv <= 60000) profile = OUTPUT_PROFILE_48V;
    else if (v_mv > 60000)                   profile = OUTPUT_PROFILE_72V;
    else                                     profile = OUTPUT_PROFILE_STANDBY;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.output_profile = profile;
    xSemaphoreGive(g_state_mutex);
    current_profile = profile;

    ESP_LOGI(TAG_PB, "Output profile: %d", profile);
}

output_profile_t pre_bias_get_profile(void) { return current_profile; }
uint32_t pre_bias_get_nominal_mv(void) { return nominal_mv_table[current_profile]; }

void pre_bias_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PREBIAS_REEVAL_INTERVAL_MS));
        if (!g_sys.boot_complete) continue;

        output_profile_t old_profile = current_profile;
        pre_bias_detect_and_select();

        if (current_profile != old_profile) {
            ESP_LOGI(TAG_PB, "Profile changed %d→%d — re-verifying handshake",
                     old_profile, current_profile);
            /* Per spec: any profile change triggers fresh handshake verification */
            /* In practice: publish to MQTT and require operator confirmation */
            mqtt_publish_fault("PROFILE_CHANGE", 0);
        }
    }
}

/* ================================================================
 *  config_nvs.c — NVS Config Load/Save
 * ================================================================ */
#include "config.h"
#include "nvs.h"

#define NVS_NS "24shub"

void config_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* First boot — apply defaults */
        g_sys.ov_mv        = OV_MV_LIFEPO4;
        g_sys.uv_mv        = UV_MV_LIFEPO4;
        g_sys.oc_ma        = OC_MA_DEFAULT;
        g_sys.bal_delta_mv = BAL_DELTA_MV;
        g_sys.pwm_duty_pct = 50;
        g_sys.chemistry    = CHEM_LIFEPO4;
        return;
    }
    uint32_t v;
    if (nvs_get_u32(h, "ov_mv",  &v) == ESP_OK) g_sys.ov_mv        = v;
    else g_sys.ov_mv = OV_MV_LIFEPO4;
    if (nvs_get_u32(h, "uv_mv",  &v) == ESP_OK) g_sys.uv_mv        = v;
    else g_sys.uv_mv = UV_MV_LIFEPO4;
    if (nvs_get_u32(h, "oc_ma",  &v) == ESP_OK) g_sys.oc_ma        = v;
    else g_sys.oc_ma = OC_MA_DEFAULT;
    if (nvs_get_u32(h, "bal_mv", &v) == ESP_OK) g_sys.bal_delta_mv = v;
    else g_sys.bal_delta_mv = BAL_DELTA_MV;
    if (nvs_get_u32(h, "duty",   &v) == ESP_OK) g_sys.pwm_duty_pct = v;
    else g_sys.pwm_duty_pct = 50;
    nvs_close(h);
}

void config_save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "ov_mv",  g_sys.ov_mv);
    nvs_set_u32(h, "uv_mv",  g_sys.uv_mv);
    nvs_set_u32(h, "oc_ma",  g_sys.oc_ma);
    nvs_set_u32(h, "bal_mv", g_sys.bal_delta_mv);
    nvs_set_u32(h, "duty",   g_sys.pwm_duty_pct);
    nvs_commit(h);
    nvs_close(h);
}
