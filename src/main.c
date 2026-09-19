/**
 * 24S Smart Hub — Main Firmware Entry Point
 * ESP32-S2-WROVER-I | Milestone 2
 * 
 * Covers:
 *  - Boot & Initialization sequence
 *  - Task scheduler (FreeRTOS)
 *  - All subsystem init calls
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "config.h"
#include "hardware_init.h"
#include "ltc6813.h"
#include "ina240.h"
#include "scp_recovery.h"
#include "third_wire.h"
#include "pwm_mode.h"
#include "rf_handshake.h"
#include "cell_monitor.h"
#include "mqtt_telemetry.h"
#include "black_box.h"
#include "wifi_manager.h"
#include "pre_bias.h"

static const char *TAG = "MAIN";

/* ---------------------------------------------------------------
 *  Global system state — shared across all modules via extern
 * -------------------------------------------------------------- */
system_state_t g_sys = {
    .gate_state       = GATE_OFF,
    .op_mode          = MODE_OFF,
    .fault_count      = 0,
    .permanent_fault  = false,
    .wifi_connected   = false,
    .handshake_done   = false,
    .boot_complete    = false,
};

SemaphoreHandle_t g_state_mutex;

/* ---------------------------------------------------------------
 *  Boot sequence — matches Figure 2 exactly
 * -------------------------------------------------------------- */
static esp_err_t run_boot_sequence(void)
{
    ESP_LOGI(TAG, "=== 24S Smart Hub Boot Sequence ===");

    /* Step 1 — Peripheral init */
    ESP_LOGI(TAG, "[1/8] Peripheral init...");
    ESP_ERROR_CHECK(hardware_init());           // GPIO, LEDC PWM, SPI, ADC
    ESP_LOGI(TAG, "      GPIO / LEDC / SPI / ADC OK");

    /* Step 2 — NVS load (config, credentials, pairing key, last mode) */
    ESP_LOGI(TAG, "[2/8] Loading NVS...");
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase — reflashing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    config_load_from_nvs();
    ESP_LOGI(TAG, "      NVS OK");

    /* Step 3 — Black Box init (must happen before any fault can be logged) */
    ESP_LOGI(TAG, "[3/8] Black Box init...");
    black_box_init();
    black_box_write_boot_event();
    ESP_LOGI(TAG, "      Black Box OK");

    /* Step 4 — LTC6813 self-test: read all 24 cell voltages */
    ESP_LOGI(TAG, "[4/8] LTC6813 self-test...");
    if (ltc6813_self_test() != ESP_OK) {
        ESP_LOGE(TAG, "LTC6813 self-test FAILED — halting in safe standby");
        black_box_write_fault(FAULT_CELL_MONITOR_FAIL, 0, 0);
        led_blink_fault();
        /* Publish fault if WiFi connects later — task handles it */
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "      LTC6813 OK — 24 cells verified");

    /* Step 5 — INA240 + TLV3501 idle check */
    ESP_LOGI(TAG, "[5/8] INA240 + TLV3501 idle check...");
    if (ina240_verify_idle() != ESP_OK) {
        ESP_LOGE(TAG, "INA240 idle check FAILED — stuck comparator or shorted sense path");
        black_box_write_fault(FAULT_INA240_FAIL, 0, 0);
        led_blink_fault();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "      INA240 idle OK");

    /* Step 6 — Wi-Fi connect (non-blocking, fallback to offline Black Box) */
    ESP_LOGI(TAG, "[6/8] Wi-Fi connect (30s timeout, offline mode if miss)...");
    wifi_manager_start();   // async — sets g_sys.wifi_connected when up

    /* Step 7 — RF Handshake (30 second window) */
    ESP_LOGI(TAG, "[7/8] Waiting for RF handshake (30s)...");
    if (rf_handshake_wait(30000) != ESP_OK) {
        ESP_LOGE(TAG, "RF handshake timeout — safe standby");
        led_blink_amber_1hz();
        return ESP_FAIL;
    }
    g_sys.handshake_done = true;
    ESP_LOGI(TAG, "      RF handshake confirmed");

    /* Step 8 — Pre-bias sense: read load-side ADC, select output profile */
    ESP_LOGI(TAG, "[8/8] Pre-bias sense...");
    pre_bias_detect_and_select();

    /* Gate enable — ISO5451DW driven by ESP32 GATE_CTRL GPIO */
    ESP_LOGI(TAG, "Gate ENABLE — 72V rail going live");
    scp_recovery_enable_gate();

    g_sys.boot_complete = true;
    ESP_LOGI(TAG, "=== Boot complete — entering main loop ===");
    return ESP_OK;
}

/* ---------------------------------------------------------------
 *  app_main
 * -------------------------------------------------------------- */
void app_main(void)
{
    g_state_mutex = xSemaphoreCreateMutex();

    /* Boot sequence — all outputs off until complete */
    gate_hold_off();   // safety: ensure gate is off before anything else

    if (run_boot_sequence() != ESP_OK) {
        /* Stay in safe standby — only monitoring + Black Box run */
        xTaskCreate(cell_monitor_task,  "cell_mon",  4096, NULL, 5, NULL);
        xTaskCreate(black_box_task,     "blackbox",  4096, NULL, 4, NULL);
        xTaskCreate(wifi_manager_task,  "wifi",      4096, NULL, 3, NULL);
        xTaskCreate(mqtt_fault_task,    "mqtt_flt",  4096, NULL, 3, NULL);
        /* Spin here — reset required to retry boot */
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* ---- Spawn all runtime tasks ---- */

    /* High priority: SCP interrupt handler state machine */
    xTaskCreate(scp_recovery_task,   "scp",       4096, NULL, 10, NULL);

    /* Third Wire GPIO interrupt + mode state machine */
    xTaskCreate(third_wire_task,     "third_wire", 4096, NULL, 8, NULL);

    /* Cell monitoring: 250ms voltage scan, 500ms temp, balancing */
    xTaskCreate(cell_monitor_task,   "cell_mon",  8192, NULL, 7, NULL);

    /* MQTT telemetry publish + subscribe command handler */
    xTaskCreate(mqtt_telemetry_task, "mqtt",      8192, NULL, 6, NULL);

    /* Black Box logger */
    xTaskCreate(black_box_task,      "blackbox",  4096, NULL, 5, NULL);

    /* Wi-Fi / 4G watchdog */
    xTaskCreate(wifi_manager_task,   "wifi",      4096, NULL, 4, NULL);

    /* Pre-bias re-evaluation every 5 seconds */
    xTaskCreate(pre_bias_task,       "prebias",   4096, NULL, 3, NULL);

    /* Idle */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
