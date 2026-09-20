#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "WIFI";
static bool g_wifi_started = false;
static bool g_sntp_started = false;

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "SMARTHUB"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "smart-hub"
#endif

static void start_sntp_once(void)
{
    if (g_sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    g_sntp_started = true;
    ESP_LOGI(TAG, "SNTP initialized");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi station starting");
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.wifi_connected = false;
            xSemaphoreGive(g_state_mutex);
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
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.wifi_connected = true;
        xSemaphoreGive(g_state_mutex);
        start_sntp_once();
        ESP_LOGI(TAG, "Wi-Fi connected; MQTT runtime can proceed");
    }
}

esp_err_t wifi_manager_start(void)
{
    if (g_wifi_started) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        /* Default STA netif may already exist after an earlier initialization. */
        ESP_LOGW(TAG, "Default STA netif already exists or could not be created");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", CONFIG_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", CONFIG_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    g_wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi manager initialized for SSID %s", CONFIG_WIFI_SSID);
    return ESP_OK;
}

void wifi_manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!g_sys.wifi_connected) {
            if (g_wifi_started) esp_wifi_connect();
            continue;
        }

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.wifi_rssi = (int8_t)ap_info.rssi;
            xSemaphoreGive(g_state_mutex);
        }
    }
}
