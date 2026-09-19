#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WIFI";

void wifi_manager_start(void)
{
    ESP_LOGI(TAG, "Wi-Fi manager stub initialized");
}

void wifi_manager_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
