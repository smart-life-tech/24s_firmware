/**
 * ltc6813.c — LTC6811-1 isoSPI Cell Monitor Driver
 *
 * Final PCB hardware: two LTC6811-1 devices in daisy-chain,
 * 12 cells per device, 24 cells total. This driver reads the actual
 * 2 x 12-cell chain rather than the stale 18-cell assumption.
 */

#include "ltc6813.h"
#include "config.h"
#include "hardware_init.h"
#include "black_box.h"
#include "mqtt_telemetry.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LTC6813";
extern spi_device_handle_t g_spi_ltc;

/* ----------------------------------------------------------------
 *  PEC (15-bit CRC) — LTC6813-1 datasheet Table 4
 * ---------------------------------------------------------------- */
static const uint16_t pec15_table[256] = {
    /* Pre-computed CRC table — standard LTC PEC15 */
    0x0000,0xC599,0xCEAB,0x0B32,0xD8CF,0x1D56,0x1664,0xD3FD,
    /* ... (full 256-entry table would be here) */
    /* For brevity we compute dynamically below */
};

static uint16_t pec15_calc(const uint8_t *data, int len)
{
    uint16_t pec = 16;  /* seed */
    for (int i = 0; i < len; i++) {
        uint8_t in = data[i] ^ (uint8_t)(pec >> 7);
        /* Standard LTC PEC15 polynomial: 0x4599 */
        for (int j = 0; j < 8; j++) {
            pec = (pec << 1) ^ ((pec & 0x4000) ? 0x4599 : 0);
        }
        pec ^= in;
    }
    return pec & 0x7FFF;
}

/* ----------------------------------------------------------------
 *  SPI transaction (isoSPI wake-up + command + PEC)
 * ---------------------------------------------------------------- */
#define CMD_ADCV   0x0360   // Start cell voltage ADC
#define CMD_RDCVA  0x0004   // Read cell voltage register group A (cells 1-3)
#define CMD_RDCVB  0x0006   // B (4-6)
#define CMD_RDCVC  0x0008   // C (7-9)
#define CMD_RDCVD  0x000A   // D (10-12)
#define CMD_RDCVE  0x0009   // E (13-15)
#define CMD_RDCVF  0x000B   // F (16-18)
#define CMD_WRCFGA 0x0001   // Write config register A
#define CMD_RDSTAT 0x0010   // Read status register A

/* Wake isoSPI by sending dummy byte */
static void ltc_wake(void)
{
    uint8_t dummy = 0xFF;
    spi_transaction_t t = {
        .tx_buffer = &dummy,
        .length    = 8,
    };
    spi_device_transmit(g_spi_ltc, &t);
    /* tWAKE = 300us minimum */
    esp_rom_delay_us(400);
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
    uint8_t tx[4] = {0};
    tx[0] = (cmd >> 8) & 0xFF;
    tx[1] = cmd & 0xFF;
    uint16_t pec = pec15_calc(tx, 2);
    tx[2] = (pec >> 8) & 0xFF;
    tx[3] = pec & 0xFF;

    /* Daisy-chain: 4 cmd bytes + (6 data + 2 PEC) × 2 ICs = 20 bytes total */
    uint8_t rx[20] = {0};
    spi_transaction_t t = {
        .tx_buffer = tx,
        .rx_buffer = rx,
        .length    = 160,   // 20 bytes × 8 bits
    };
    esp_err_t ret = spi_device_transmit(g_spi_ltc, &t);
    if (ret != ESP_OK) return ret;

    /* IC2 data comes first in daisy-chain, IC1 last */
    memcpy(data_ic2, rx + 4,  6);
    memcpy(data_ic1, rx + 12, 6);

    /* Verify PEC for both */
    uint16_t pec_rx2 = (rx[10] << 8) | rx[11];
    uint16_t pec_rx1 = (rx[18] << 8) | rx[19];
    if (pec15_calc(data_ic2, 6) != pec_rx2) return ESP_ERR_INVALID_CRC;
    if (pec15_calc(data_ic1, 6) != pec_rx1) return ESP_ERR_INVALID_CRC;

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
static int pec_fail_count[2] = {0, 0};

esp_err_t ltc6813_read_all_cells(uint16_t *cell_mv_out)
{
    ltc_wake();

    /* Start ADC conversion across all cells in the 2 x LTC6811-1 chain. */
    ltc_send_command(CMD_ADCV);
    vTaskDelay(pdMS_TO_TICKS(2));

    uint16_t group_cmds[4] = {CMD_RDCVA, CMD_RDCVB, CMD_RDCVC, CMD_RDCVD};
    uint16_t cells[CELL_COUNT] = {0};
    int out_idx = 0;

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

        cells[out_idx++] = d1_a;
        cells[out_idx++] = d1_b;
        cells[out_idx++] = d1_c;
        cells[out_idx++] = d2_a;
        cells[out_idx++] = d2_b;
        cells[out_idx++] = d2_c;
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
esp_err_t ltc6813_self_test(void)
{
    uint16_t cells[CELL_COUNT];
    esp_err_t ret = ltc6813_read_all_cells(cells);
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
 *  Passive cell balancing
 *  Turns on BSS308PE FET for cells >30mV above average
 *  Turns off when <15mV above average
 * ---------------------------------------------------------------- */
#define BSS308_GPIO_BASE   GPIO_NUM_20   // GPIO 20-43 for 24 cells

static void balancing_update(const uint16_t *cells)
{
    /* Calculate pack average */
    uint32_t sum = 0;
    for (int i = 0; i < CELL_COUNT; i++) sum += cells[i];
    uint16_t avg = sum / CELL_COUNT;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t bal_thresh = g_sys.bal_delta_mv;
    xSemaphoreGive(g_state_mutex);

    for (int i = 0; i < CELL_COUNT; i++) {
        int32_t delta = (int32_t)cells[i] - avg;
        bool currently_balancing = g_sys.cell_balancing[i];

        bool should_balance;
        if (!currently_balancing) {
            should_balance = delta > (int32_t)bal_thresh;  // 30mV trigger
        } else {
            should_balance = delta > (int32_t)(BAL_STOP_MV);  // 15mV stop
        }

        if (should_balance != currently_balancing) {
            gpio_set_level(BSS308_GPIO_BASE + i, should_balance ? 1 : 0);
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.cell_balancing[i] = should_balance;
            xSemaphoreGive(g_state_mutex);
            if (should_balance) {
                ESP_LOGD(TAG, "Balancing ON cell %d (delta %d mV)", i+1, delta);
            }
        }
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
    if (v <= 0.0f || v >= 1.0f) return 25.0f;

    float r_ntc = 10000.0f * v / (1.0f - v);
    float temp_k = 1.0f / (1.0f / 298.15f + logf(r_ntc / 10000.0f) / 3950.0f);
    return temp_k - 273.15f;
}

/* ----------------------------------------------------------------
 *  Cell threshold checks (OV/UV)
 * ---------------------------------------------------------------- */
static void check_thresholds(const uint16_t *cells)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t ov = g_sys.ov_mv;
    uint32_t uv = g_sys.uv_mv;
    xSemaphoreGive(g_state_mutex);

    for (int i = 0; i < CELL_COUNT; i++) {
        if (cells[i] >= ov) {
            ESP_LOGW(TAG, "Cell %d OV: %d mV", i+1, cells[i]);
            black_box_write_cell_threshold(FAULT_CELL_OV, i, cells[i]);
            mqtt_publish_fault("CELL_OV", 0);
        } else if (cells[i] <= uv) {
            ESP_LOGW(TAG, "Cell %d UV: %d mV", i+1, cells[i]);
            black_box_write_cell_threshold(FAULT_CELL_UV, i, cells[i]);
            mqtt_publish_fault("CELL_UV", 0);
        }
    }
}

/* ----------------------------------------------------------------
 *  Cell monitor task
 * ---------------------------------------------------------------- */
void cell_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Cell monitor task running");
    uint32_t temp_tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CELL_SCAN_INTERVAL_MS));

        /* Voltage scan */
        uint16_t cells[CELL_COUNT];
        if (ltc6813_read_all_cells(cells) == ESP_OK) {
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
            float temps[4];
            float sum = 0;
            for (int s = 0; s < 4; s++) {
                temps[s] = ntc_read_temperature(s);
                sum += temps[s];
            }
            float avg = sum / 4.0f;

            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.temp_avg_c = avg;
            memcpy(g_sys.temp_sensors_c, temps, sizeof(temps));
            xSemaphoreGive(g_state_mutex);

            if (avg >= TEMP_SHUTDOWN_C) {
                ESP_LOGE(TAG, "TEMP SHUTDOWN: %.1f°C", avg);
                black_box_write_temp_alert(0xFF, avg);
                mqtt_publish_fault("TEMP_SHUTDOWN", 0);
                /* Graceful gate shutdown */
                gpio_set_level(PIN_GATE_CTRL, 0);
            } else if (avg >= TEMP_WARN_C) {
                ESP_LOGW(TAG, "Temp warning: %.1f°C", avg);
                black_box_write_temp_alert(0xFE, avg);
                mqtt_publish_fault("TEMP_WARN", 0);
            }
        }
    }
}
