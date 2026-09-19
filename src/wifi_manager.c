#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "WIFI";

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "SMARTHUB"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "smart-hub"
#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi station starting");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.wifi_connected = false;
            xSemaphoreGive(g_state_mutex);
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.wifi_connected = true;
            xSemaphoreGive(g_state_mutex);
            ESP_LOGI(TAG, "Wi-Fi connected; MQTT runtime can proceed");
            break;
        default:
            break;
        }
    }
}

void wifi_manager_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    (void)sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", CONFIG_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", CONFIG_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wi-Fi manager initialized for SSID %s", CONFIG_WIFI_SSID);
}

void wifi_manager_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (g_sys.wifi_connected) {
            esp_wifi_connect();
        }
    }
}
