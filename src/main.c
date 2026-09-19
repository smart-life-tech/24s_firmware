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
#include "ltc6811.h"
#include "ina240.h"
#include "scp_recovery.h"
#include "third_wire.h"
#include "pwm_mode.h"
#include "mqtt_telemetry.h"
#include "black_box.h"
#include "wifi_manager.h"

static const char *TAG = "MAIN";

system_state_t g_sys = {
    .gate_state       = GATE_OFF,
    .op_mode          = MODE_OFF,
    .fault_count      = 0,
    .permanent_fault  = false,
    .wifi_connected   = false,
    .boot_complete    = false,
    .pwm_remote_override = false,
};

SemaphoreHandle_t g_state_mutex;

static esp_err_t run_boot_sequence(void)
{
    ESP_LOGI(TAG, "=== 24S Smart Hub Boot Sequence ===");

    ESP_LOGI(TAG, "[1/5] Peripheral init...");
    ESP_ERROR_CHECK(hardware_init());
    ESP_LOGI(TAG, "      GPIO / LEDC / SPI / ADC OK");

    ESP_LOGI(TAG, "[2/5] Loading NVS...");
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase — reflashing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    config_load_from_nvs();
    ESP_LOGI(TAG, "      NVS OK");

    ESP_LOGI(TAG, "[3/5] Black Box init...");
    black_box_init();
    black_box_write_boot_event();
    ESP_LOGI(TAG, "      Black Box OK");

    ESP_LOGI(TAG, "[4/5] LTC6811-1 self-test...");
    if (ltc6811_self_test() != ESP_OK) {
        ESP_LOGE(TAG, "LTC6811-1 self-test FAILED — safe standby");
        black_box_write_fault(FAULT_CELL_MONITOR_FAIL, 0, 0);
        led_blink_fault();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "      LTC6811-1 OK");

    ESP_LOGI(TAG, "[5/5] INA240 idle check...");
    if (ina240_verify_idle() != ESP_OK) {
        ESP_LOGE(TAG, "INA240 idle check FAILED");
        black_box_write_fault(FAULT_INA240_FAIL, 0, 0);
        led_blink_fault();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "      INA240 idle OK");

    wifi_manager_start();
    g_sys.boot_complete = true;
    ESP_LOGI(TAG, "=== Boot complete — entering main loop ===");
    return ESP_OK;
}

void app_main(void)
{
    g_state_mutex = xSemaphoreCreateMutex();
    gate_hold_off();

    if (run_boot_sequence() != ESP_OK) {
        xTaskCreate(cell_monitor_task,  "cell_mon",  4096, NULL, 5, NULL);
        xTaskCreate(black_box_task,     "blackbox",  4096, NULL, 4, NULL);
        xTaskCreate(wifi_manager_task,  "wifi",      4096, NULL, 3, NULL);
        xTaskCreate(mqtt_fault_task,    "mqtt_flt",  4096, NULL, 3, NULL);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    xTaskCreate(scp_recovery_task,   "scp",       4096, NULL, 10, NULL);
    xTaskCreate(third_wire_task,     "third_wire", 4096, NULL, 8, NULL);
    xTaskCreate(cell_monitor_task,   "cell_mon",  8192, NULL, 7, NULL);
    xTaskCreate(mqtt_telemetry_task, "mqtt",      8192, NULL, 6, NULL);
    xTaskCreate(black_box_task,      "blackbox",  4096, NULL, 5, NULL);
    xTaskCreate(wifi_manager_task,   "wifi",      4096, NULL, 4, NULL);

    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
