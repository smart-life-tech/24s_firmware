/**
 * ltc6811.c — LTC6811-1 isoSPI Cell Monitor Driver
 *
 * Final PCB hardware: two LTC6811-1 devices in daisy-chain,
 * 12 cells per device, 24 cells total. This driver reads the actual
 * 2 x 12-cell chain rather than the stale 18-cell assumption.
 */

#include "ltc6811.h"
#include "config.h"
#include "hardware_init.h"
#include "black_box.h"
#include "mqtt_telemetry.h"
#include "ina240.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LTC6811";
extern spi_device_handle_t g_spi_ltc;

static bool g_last_cell_ov[CELL_COUNT] = {false};
static bool g_last_cell_uv[CELL_COUNT] = {false};
static bool g_last_oc_fault = false;
static bool g_last_pack_ov_fault = false;
static bool g_last_temp_invalid[NTC_COUNT] = {false};
static bool g_last_temp_warn[NTC_COUNT] = {false};

/* ----------------------------------------------------------------
 *  PEC (15-bit CRC) — LTC6811-1 reference implementation
 * ---------------------------------------------------------------- */
static uint16_t pec15_table[256];
static void init_pec15_table(void)
{
    static bool initialized = false;
    if (initialized) return;

    for (int i = 0; i < 256; i++) {
        uint16_t remainder = (uint16_t)(i << 7);
        for (int bit = 0; bit < 8; bit++) {
            if (remainder & 0x4000) {
                remainder = (uint16_t)((remainder << 1) ^ 0x4599);
            } else {
                remainder = (uint16_t)(remainder << 1);
            }
            remainder &= 0x7FFF;
        }
        pec15_table[i] = remainder;
    }
    initialized = true;
}

static uint16_t pec15_calc(const uint8_t *data, int len)
{
    init_pec15_table();
    uint16_t remainder = 16;
    for (int i = 0; i < len; i++) {
        uint8_t index = (uint8_t)(((remainder >> 7) ^ data[i]) & 0xFF);
        remainder = (uint16_t)(((remainder << 8) ^ pec15_table[index]) & 0x7FFF);
    }
    /* LTC6811 PEC15 result is returned with the low bit shifted to maintain the
     * 15-bit CRC format used by the device; the LSB must be zero in the final PEC word. */
    return (uint16_t)(remainder << 1);
}

/* ----------------------------------------------------------------
 *  SPI transaction (isoSPI wake-up + command + PEC)
 * ---------------------------------------------------------------- */
#define CMD_ADCV   0x0260   // Start cell voltage ADC (LTC6811 command)
#define CMD_RDCFGA 0x0002   // Read config register A
#define CMD_RDCVA  0x0004   // Read cell voltage register group A (cells 1-3)
#define CMD_RDCVB  0x0006   // B (4-6)
#define CMD_RDCVC  0x0008   // C (7-9)
#define CMD_RDCVD  0x000A   // D (10-12)
#define CMD_WRCFGA 0x0001   // Write config register A
#define CMD_RDSTAT 0x0010   // Read status register A

/* Wake both devices in the daisy chain. The 2-device chain requires a more
 * conservative wake than a single device; repeated dummy bytes ensure the
 * downstream device has also exited standby before the next command.
 */
static void ltc_wake(void)
{
    uint8_t dummy = 0xFF;
    for (int i = 0; i < 3; i++) {
        spi_transaction_t t = {
            .tx_buffer = &dummy,
            .length    = 8,
        };
        spi_device_transmit(g_spi_ltc, &t);
        esp_rom_delay_us(300);
    }
}

static esp_err_t ltc_send_command(uint16_t cmd)
{
    uint8_t buf[4];
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;
    uint16_t pec = pec15_calc(buf, 2);
    buf[2] = (pec >> 8) & 0xFF;
    buf[3] = pec & 0xFF;

    spi_transaction_t t = {
        .tx_buffer = buf,
        .length    = 32,
    };
    return spi_device_transmit(g_spi_ltc, &t);
}

/* Read 6-byte register group + 2-byte PEC — for both ICs in daisy-chain */
static esp_err_t ltc_read_register(uint16_t cmd, uint8_t *data_ic1,
                                    uint8_t *data_ic2)
{
    uint8_t tx[20] = {0};
    uint8_t rx[20] = {0};
    tx[0] = (cmd >> 8) & 0xFF;
    tx[1] = cmd & 0xFF;
    uint16_t pec = pec15_calc(tx, 2);
    tx[2] = (pec >> 8) & 0xFF;
    tx[3] = pec & 0xFF;

    /* 4-byte command + 8 bytes IC2 + 8 bytes IC1 = 20 bytes */
    spi_transaction_t t = {
        .tx_buffer = tx,
        .rx_buffer = rx,
        .length    = sizeof(tx) * 8,
    };
    esp_err_t ret = spi_device_transmit(g_spi_ltc, &t);
    if (ret != ESP_OK) return ret;

    uint8_t raw_ic2[8] = {0};
    uint8_t raw_ic1[8] = {0};
    memcpy(raw_ic2, rx + 4, 8);
    memcpy(raw_ic1, rx + 12, 8);
    memcpy(data_ic2, raw_ic2, 6);
    memcpy(data_ic1, raw_ic1, 6);

    uint16_t pec_rx2 = (raw_ic2[6] << 8) | raw_ic2[7];
    uint16_t pec_rx1 = (raw_ic1[6] << 8) | raw_ic1[7];
    if (pec15_calc(raw_ic2, 6) != pec_rx2) return ESP_ERR_INVALID_CRC;
    if (pec15_calc(raw_ic1, 6) != pec_rx1) return ESP_ERR_INVALID_CRC;

    return ESP_OK;
}

static uint8_t g_cfg_u19[6] = {0};
static uint8_t g_cfg_u23[6] = {0};
static bool g_cfg_loaded = false;

static esp_err_t ltc6811_load_config_snapshot(void)
{
    uint8_t cfg_u19[6] = {0};
    uint8_t cfg_u23[6] = {0};
    esp_err_t ret = ltc_read_register(CMD_RDCFGA, cfg_u19, cfg_u23);
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(g_cfg_u19, cfg_u19, sizeof(g_cfg_u19));
    memcpy(g_cfg_u23, cfg_u23, sizeof(g_cfg_u23));
    g_cfg_loaded = true;
    return ESP_OK;
}

/* ----------------------------------------------------------------
 *  Parse voltage register group (3 cells per group)
 *  LSB = 100µV → value in mV = raw × 0.1
 * ---------------------------------------------------------------- */
static void parse_voltage_group(const uint8_t *reg, uint16_t *cell_a,
                                 uint16_t *cell_b, uint16_t *cell_c)
{
    uint16_t raw_a = (reg[1] << 8) | reg[0];
    uint16_t raw_b = (reg[3] << 8) | reg[2];
    uint16_t raw_c = (reg[5] << 8) | reg[4];
    /* raw × 100µV → mV = raw / 10 */
    *cell_a = raw_a / 10;
    *cell_b = raw_b / 10;
    *cell_c = raw_c / 10;
}

/* ----------------------------------------------------------------
 *  Read all 24 cell voltages
 * ---------------------------------------------------------------- */
esp_err_t ltc6811_read_all_cells(uint16_t *cell_mv_out)
{
    ltc_wake();

    /* Start ADC conversion across the 2 x LTC6811-1 chain and allow enough
     * time for the worst-case REFUP + conversion window in normal mode. */
    esp_err_t ret = ltc_send_command(CMD_ADCV);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADCV command failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(7));

    uint16_t group_cmds[4] = {CMD_RDCVA, CMD_RDCVB, CMD_RDCVC, CMD_RDCVD};
    uint16_t cells[CELL_COUNT] = {0};
    int out_idx = 0;

    uint16_t ic1_groups[4][3] = {{0}};
    uint16_t ic2_groups[4][3] = {{0}};

    for (int g = 0; g < 4; g++) {
        uint8_t ic1[6] = {0};
        uint8_t ic2[6] = {0};

        esp_err_t r = ltc_read_register(group_cmds[g], ic1, ic2);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "Cell group %d read failed (CRC/error)", g);
            return r;
        }

        uint16_t d1_a, d1_b, d1_c;
        uint16_t d2_a, d2_b, d2_c;
        parse_voltage_group(ic1, &d1_a, &d1_b, &d1_c);
        parse_voltage_group(ic2, &d2_a, &d2_b, &d2_c);

        ic1_groups[g][0] = d1_a;
        ic1_groups[g][1] = d1_b;
        ic1_groups[g][2] = d1_c;
        ic2_groups[g][0] = d2_a;
        ic2_groups[g][1] = d2_b;
        ic2_groups[g][2] = d2_c;
    }

    for (int g = 0; g < 4; g++) {
        cells[out_idx++] = ic1_groups[g][0];
        cells[out_idx++] = ic1_groups[g][1];
        cells[out_idx++] = ic1_groups[g][2];
    }

    for (int g = 0; g < 4; g++) {
        cells[out_idx++] = ic2_groups[g][0];
        cells[out_idx++] = ic2_groups[g][1];
        cells[out_idx++] = ic2_groups[g][2];
    }

    if (out_idx != CELL_COUNT) {
        ESP_LOGE(TAG, "Expected %d cell values, got %d", CELL_COUNT, out_idx);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(cell_mv_out, cells, CELL_COUNT * sizeof(uint16_t));
    return ESP_OK;
}

/* ----------------------------------------------------------------
 *  Self-test: read all cells and verify plausible range
 * ---------------------------------------------------------------- */
esp_err_t ltc6811_self_test(void)
{
    uint16_t cells[CELL_COUNT];
    esp_err_t ret = ltc6811_read_all_cells(cells);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Self-test: isoSPI read failed");
        return ret;
    }
    for (int i = 0; i < CELL_COUNT; i++) {
        if (cells[i] < 1000 || cells[i] > 5000) {
            ESP_LOGE(TAG, "Cell %d out of range: %d mV", i+1, cells[i]);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "Self-test: all 24 cells in range");
    return ESP_OK;
}

/* ----------------------------------------------------------------
 *  Passive cell balancing via LTC6811 DCC1..DCC12 outputs
 *
 *  The final PCB does not use ESP32 GPIOs for individual cell balancing.
 *  Each LTC6811 Sx pin drives an external BSS308PE P-MOSFET through a
 *  3.3 kΩ gate resistor, and each MOSFET discharges its cell through a
 *  100 Ω resistor. The firmware therefore controls the DCC bits in the
 *  LTC6811 configuration register rather than toggling GPIOs.
 *
 *  Balancing current is fixed by the 100 Ω bleed resistor: at a typical
 *  3.65 V cell (LiFePO4 OV threshold) this is ~36.5 mA. This is passive
 *  balancing only; any requirement for ~1 A of balancing current cannot
 *  be met by firmware changes on the final PCB and would require a
 *  hardware redesign (lower bleed resistance and/or active balancing).
 * ---------------------------------------------------------------- */
static uint16_t g_balance_mask_u19 = 0U;
static uint16_t g_balance_mask_u23 = 0U;
static TickType_t g_last_balance_refresh_tick = 0;

static esp_err_t ltc6811_write_config(const uint8_t cfg_u19[6], const uint8_t cfg_u23[6])
{
    ltc_wake();

    uint8_t tx[20] = {0};

    tx[0] = (CMD_WRCFGA >> 8) & 0xFFU;
    tx[1] = CMD_WRCFGA & 0xFFU;
    uint16_t pec = pec15_calc(tx, 2);
    tx[2] = (pec >> 8) & 0xFFU;
    tx[3] = pec & 0xFFU;

    /* The stacked LTC6811 is transmitted first, followed by the primary device.
     * This matches the daisy-chain ordering used for the 24-cell stack.
     */
    memcpy(&tx[4], cfg_u23, 6);
    pec = pec15_calc(&tx[4], 6);
    tx[10] = (pec >> 8) & 0xFFU;
    tx[11] = pec & 0xFFU;

    memcpy(&tx[12], cfg_u19, 6);
    pec = pec15_calc(&tx[12], 6);
    tx[18] = (pec >> 8) & 0xFFU;
    tx[19] = pec & 0xFFU;

    spi_transaction_t t = {
        .tx_buffer = tx,
        .length    = sizeof(tx) * 8,
    };

    return spi_device_transmit(g_spi_ltc, &t);
}

static esp_err_t ltc6811_set_balance_masks(uint16_t mask_u19, uint16_t mask_u23, bool force_write)
{
    if (!g_cfg_loaded) {
        esp_err_t ret = ltc6811_load_config_snapshot();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (!force_write && mask_u19 == g_balance_mask_u19 && mask_u23 == g_balance_mask_u23) {
        return ESP_OK;
    }

    uint8_t cfg_u19[6];
    uint8_t cfg_u23[6];
    memcpy(cfg_u19, g_cfg_u19, sizeof(cfg_u19));
    memcpy(cfg_u23, g_cfg_u23, sizeof(cfg_u23));

    /* DCC bits are stored in CFGR4 and CFGR5.
     * Preserve the other configuration bytes while only updating the DCC mask.
     * CFGR4 bits[0:7] = DCC1..DCC8
     * CFGR5 bits[0:3] = DCC9..DCC12, bits[4:7] = DCTO
     */
    cfg_u19[4] = (uint8_t)(mask_u19 & 0xFFU);
    cfg_u19[5] = (uint8_t)(((cfg_u19[5] & 0xF0U) | ((mask_u19 >> 8) & 0x0FU)) & 0xFFU);

    cfg_u23[4] = (uint8_t)(mask_u23 & 0xFFU);
    cfg_u23[5] = (uint8_t)(((cfg_u23[5] & 0xF0U) | ((mask_u23 >> 8) & 0x0FU)) & 0xFFU);

    esp_err_t ret = ltc6811_write_config(cfg_u19, cfg_u23);
    if (ret == ESP_OK) {
        memcpy(g_cfg_u19, cfg_u19, sizeof(g_cfg_u19));
        memcpy(g_cfg_u23, cfg_u23, sizeof(g_cfg_u23));
        g_balance_mask_u19 = mask_u19;
        g_balance_mask_u23 = mask_u23;
        g_last_balance_refresh_tick = xTaskGetTickCount();
    }
    return ret;
}

static void balancing_update(const uint16_t *cells)
{
    if (cells == NULL) {
        ESP_LOGW(TAG, "Balancing disabled: no valid cell set available");
        return;
    }

    uint32_t total_mv = 0U;
    for (int i = 0; i < CELL_COUNT; i++) total_mv += cells[i];
    uint32_t avg_mv = total_mv / CELL_COUNT;
    uint32_t bal_delta = g_sys.bal_delta_mv ? g_sys.bal_delta_mv : BAL_DELTA_MV;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    bool inhibit_balancing = g_sys.fault_active || g_sys.permanent_fault;
    xSemaphoreGive(g_state_mutex);

    uint16_t old_u19 = g_balance_mask_u19;
    uint16_t old_u23 = g_balance_mask_u23;
    uint16_t mask_u19 = old_u19;
    uint16_t mask_u23 = old_u23;

    for (int i = 0; i < CELL_COUNT; i++) {
        bool currently_on = (i < 12) ? ((mask_u19 >> i) & 1U) : ((mask_u23 >> (i - 12)) & 1U);
        bool turn_on = cells[i] > (avg_mv + bal_delta);
        bool turn_off = cells[i] < (avg_mv + BAL_STOP_MV);
        bool enable = inhibit_balancing ? false : (currently_on ? !turn_off : turn_on);

        if (i < 12) {
            if (enable) mask_u19 |= (uint16_t)(1U << i);
            else        mask_u19 &= (uint16_t)~(1U << i);
        } else {
            int n = i - 12;
            if (enable) mask_u23 |= (uint16_t)(1U << n);
            else        mask_u23 &= (uint16_t)~(1U << n);
        }
    }

    bool force_refresh = (xTaskGetTickCount() - g_last_balance_refresh_tick) >= pdMS_TO_TICKS(1000);
    bool changed = (mask_u19 != old_u19) || (mask_u23 != old_u23);
    esp_err_t ret = ltc6811_set_balance_masks(mask_u19, mask_u23, force_refresh);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Balance config write failed: %s", esp_err_to_name(ret));
        return;
    }

    if (changed || force_refresh) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        for (int i = 0; i < CELL_COUNT; i++) {
            g_sys.cell_balancing[i] = (i < 12) ? ((mask_u19 >> i) & 1U) : ((mask_u23 >> (i - 12)) & 1U);
        }
        xSemaphoreGive(g_state_mutex);
    }
}

/* ----------------------------------------------------------------
 *  NTC thermistor read (GPIO-muxed, averaged over 4 samples)
 * ---------------------------------------------------------------- */
static float ntc_read_temperature(int sensor_idx)
{
    static const adc1_channel_t ntc_channels[NTC_COUNT] = {
        PIN_NTC_1,
        PIN_NTC_2,
        PIN_NTC_3,
        PIN_NTC_4,
    };

    uint32_t sum = 0;
    for (int s = 0; s < 4; s++) {
        sum += adc_read_mv(ntc_channels[sensor_idx]);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    float v_mv = sum / 4.0f;
    float v = v_mv / 3300.0f;
    if (!isfinite(v) || v <= 0.0f || v >= 1.0f) {
        return NAN;
    }

    float r_ntc = 10000.0f * v / (1.0f - v);
    float temp_k = 1.0f / (1.0f / 298.15f + logf(r_ntc / 10000.0f) / 3950.0f);
    float temp_c = temp_k - 273.15f;
    if (!isfinite(temp_c) || temp_c < -50.0f || temp_c > 150.0f) {
        return NAN;
    }
    return temp_c;
}

/* ----------------------------------------------------------------
 *  Cell threshold checks (OV/UV)
 * ---------------------------------------------------------------- */
void ltc6811_reset_fault_latches(void)
{
    for (int i = 0; i < CELL_COUNT; i++) {
        g_last_cell_ov[i] = false;
        g_last_cell_uv[i] = false;
    }
    g_last_oc_fault = false;
    g_last_pack_ov_fault = false;
}

static void check_thresholds(const uint16_t *cells)
{
    uint32_t ov;
    uint32_t uv;
    uint32_t oc_ma;
    int32_t pack_current_ma;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    ov = g_sys.ov_mv;
    uv = g_sys.uv_mv;
    oc_ma = g_sys.oc_ma;
    pack_current_ma = g_sys.pack_current_ma;
    xSemaphoreGive(g_state_mutex);

    uint32_t pack_total_mv = 0U;
    bool any_ov = false;
    bool any_uv = false;

    for (int i = 0; i < CELL_COUNT; i++) {
        pack_total_mv += cells[i];

        bool ov_now = (cells[i] >= ov);
        bool uv_now = (cells[i] <= uv);
        any_ov |= ov_now;
        any_uv |= uv_now;

        if (ov_now && !g_last_cell_ov[i]) {
            protection_set_fault(PROT_FAULT_CELL_OV, false);
            gate_hold_off();
            ESP_LOGW(TAG, "Cell %d OV: %d mV", i + 1, cells[i]);
            black_box_write_cell_threshold(FAULT_CELL_OV, (uint8_t)i, cells[i]);
            mqtt_publish_fault("CELL_OV", 0);
        }
        if (uv_now && !g_last_cell_uv[i]) {
            protection_set_fault(PROT_FAULT_CELL_UV, false);
            gate_hold_off();
            ESP_LOGW(TAG, "Cell %d UV: %d mV", i + 1, cells[i]);
            black_box_write_cell_threshold(FAULT_CELL_UV, (uint8_t)i, cells[i]);
            mqtt_publish_fault("CELL_UV", 0);
        }

        g_last_cell_ov[i] = ov_now;
        g_last_cell_uv[i] = uv_now;
    }

    if (!any_ov) protection_clear_fault(PROT_FAULT_CELL_OV);
    if (!any_uv) protection_clear_fault(PROT_FAULT_CELL_UV);

    bool oc_now = (oc_ma > 0U && pack_current_ma > (int32_t)oc_ma);
    if (oc_now && !g_last_oc_fault) {
        protection_set_fault(PROT_FAULT_OVERCURRENT, false);
        gate_hold_off();
        ESP_LOGE(TAG, "Over-current trip: %ld mA > %u mA", (long)pack_current_ma, oc_ma);
        black_box_write_fault(FAULT_OVERCURRENT, pack_current_ma, pack_total_mv);
        mqtt_publish_fault("OC_LIMIT", pack_current_ma);
    }
    if (!oc_now) protection_clear_fault(PROT_FAULT_OVERCURRENT);
    g_last_oc_fault = oc_now;

    bool pack_ov_now = (pack_total_mv >= PACK_CUTOFF_MV);
    if (pack_ov_now && !g_last_pack_ov_fault) {
        protection_set_fault(PROT_FAULT_PACK_OV, false);
        gate_hold_off();
        ESP_LOGE(TAG, "Pack cutoff reached: %u mV >= %u mV", pack_total_mv, PACK_CUTOFF_MV);
        black_box_write_fault(FAULT_PACK_OV, 0, pack_total_mv);
        mqtt_publish_fault("PACK_OV", 0);
    }
    if (!pack_ov_now) protection_clear_fault(PROT_FAULT_PACK_OV);
    g_last_pack_ov_fault = pack_ov_now;
}

bool ltc6811_protection_conditions_clear(void)
{
    uint16_t cells[CELL_COUNT];
    uint32_t ov, uv, oc_ma, pack_v;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    memcpy(cells, g_sys.cell_mv, sizeof(cells));
    ov = g_sys.ov_mv;
    uv = g_sys.uv_mv;
    oc_ma = g_sys.oc_ma;
    pack_v = g_sys.pack_voltage_mv;
    xSemaphoreGive(g_state_mutex);

    if (gpio_get_level(PIN_FAULT_N) == 0) return false;
    if (pack_v >= PACK_CUTOFF_MV) return false;

    int32_t current_ma = ina240_read_current_ma();
    if (oc_ma > 0U && current_ma > (int32_t)oc_ma) return false;

    for (int i = 0; i < CELL_COUNT; i++) {
        if (cells[i] >= ov || cells[i] <= uv) return false;
    }

    /* Re-sample every NTC so reset cannot clear a latched sensor fault using a
     * stale 0°C placeholder from an earlier invalid reading. */
    for (int i = 0; i < NTC_COUNT; i++) {
        float temp_c = ntc_read_temperature(i);
        if (!isfinite(temp_c) || temp_c >= TEMP_WARN_C) return false;
    }

    return true;
}

/* ----------------------------------------------------------------
 *  Cell monitor task
 * ---------------------------------------------------------------- */
void cell_monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Cell monitor task running");
    uint32_t temp_tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CELL_SCAN_INTERVAL_MS));

        /* Voltage scan */
        uint16_t cells[CELL_COUNT];
        if (ltc6811_read_all_cells(cells) == ESP_OK) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            memcpy(g_sys.cell_mv, cells, sizeof(cells));

            /* Pack voltage = sum of all cells */
            uint32_t total = 0;
            for (int i = 0; i < CELL_COUNT; i++) total += cells[i];
            g_sys.pack_voltage_mv = total;
            xSemaphoreGive(g_state_mutex);

            balancing_update(cells);
            check_thresholds(cells);
        }

        /* Temperature scan every 500ms */
        temp_tick += CELL_SCAN_INTERVAL_MS;
        if (temp_tick >= TEMP_SCAN_INTERVAL_MS) {
            temp_tick = 0;
            float temps[NTC_COUNT];
            float sum = 0.0f;
            bool any_invalid = false;
            bool any_shutdown = false;

            for (int s = 0; s < NTC_COUNT; s++) {
                temps[s] = ntc_read_temperature(s);

                if (!isfinite(temps[s])) {
                    any_invalid = true;
                    if (!g_last_temp_invalid[s]) {
                        ESP_LOGE(TAG, "NTC sensor %d invalid: open/short or ADC error", s + 1);
                        black_box_write_temp_sensor_fault((uint8_t)s);
                        mqtt_publish_fault("TEMP_SENSOR_FAIL", 0);
                    }
                    g_last_temp_invalid[s] = true;
                    temps[s] = 0.0f;
                } else {
                    g_last_temp_invalid[s] = false;
                }

                bool warn_now = (temps[s] >= TEMP_WARN_C);
                bool shutdown_now = (temps[s] >= TEMP_SHUTDOWN_C);
                any_shutdown |= shutdown_now;

                if (warn_now && !g_last_temp_warn[s] && !shutdown_now) {
                    ESP_LOGW(TAG, "Temp warning: sensor %d at %.1f°C", s + 1, temps[s]);
                    black_box_write_temp_alert((uint8_t)s, temps[s]);
                    mqtt_publish_fault("TEMP_WARN", 0);
                }
                g_last_temp_warn[s] = warn_now;
                sum += temps[s];
            }

            float avg = sum / (float)NTC_COUNT;

            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.temp_avg_c = avg;
            memcpy(g_sys.temp_sensors_c, temps, sizeof(temps));
            xSemaphoreGive(g_state_mutex);

            if (any_invalid) {
                protection_set_fault(PROT_FAULT_TEMP_SENSOR, true);
                gate_hold_off();
            }

            if (any_shutdown) {
                bool was_latched = protection_has_fault(PROT_FAULT_TEMP_SHUTDOWN);
                protection_set_fault(PROT_FAULT_TEMP_SHUTDOWN, true);
                gate_hold_off();
                if (!was_latched) {
                    for (int s = 0; s < NTC_COUNT; s++) {
                        if (temps[s] >= TEMP_SHUTDOWN_C) {
                            ESP_LOGE(TAG, "TEMP SHUTDOWN: sensor %d at %.1f°C", s + 1, temps[s]);
                            black_box_write_temp_alert((uint8_t)s, temps[s]);
                            mqtt_publish_fault("TEMP_SHUTDOWN", 0);
                            break;
                        }
                    }
                }
            }
        }
    }

}
