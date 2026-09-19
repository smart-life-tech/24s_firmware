/**
 * mqtt_telemetry.c — Bidirectional MQTT JSON Telemetry (Section 9)
 *
 * PUBLISH:
 *   hub/{id}/telemetry   every 5s   — full pack state
 *   hub/{id}/cells       every 30s  — 24 cell voltages + balancing flags
 *   hub/{id}/faults      on-event   — fault type, timestamp, peak current
 *   hub/{id}/blackbox    on-reconnect — buffered offline records
 *
 * SUBSCRIBE:
 *   hub/{id}/cmd/gate         {"action": "ON"} / {"action": "OFF"}
 *   hub/{id}/cmd/pwm          {"duty": 50}
 *   hub/{id}/cmd/reset_fault  {"confirm": true}
 *   hub/{id}/cmd/config       {"ov_mv": 3650, "uv_mv": 2500, ...}
 */

#include "mqtt_telemetry.h"
#include "config.h"
#include "third_wire.h"
#include "scp_recovery.h"
#include "black_box.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT";

#ifndef CONFIG_DEVICE_ID
#define CONFIG_DEVICE_ID "24S-HUB-001"
#endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtt://broker.local:1883"
#endif

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool                     mqtt_connected = false;
static EventGroupHandle_t       mqtt_evt_grp;
#define MQTT_CONNECTED_BIT  BIT0

/* ----------------------------------------------------------------
 *  Topic builders
 * ---------------------------------------------------------------- */
static void make_topic(char *buf, size_t len, const char *suffix)
{
    snprintf(buf, len, "hub/%s/%s", CONFIG_DEVICE_ID, suffix);
}

/* ----------------------------------------------------------------
 *  MQTT event handler
 * ---------------------------------------------------------------- */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        mqtt_connected = true;
        xEventGroupSetBits(mqtt_evt_grp, MQTT_CONNECTED_BIT);

        /* Subscribe to all command topics */
        char topic[64];
        make_topic(topic, sizeof(topic), "cmd/gate");
        esp_mqtt_client_subscribe(mqtt_client, topic, 1);
        make_topic(topic, sizeof(topic), "cmd/pwm");
        esp_mqtt_client_subscribe(mqtt_client, topic, 1);
        make_topic(topic, sizeof(topic), "cmd/reset_fault");
        esp_mqtt_client_subscribe(mqtt_client, topic, 1);
        make_topic(topic, sizeof(topic), "cmd/config");
        esp_mqtt_client_subscribe(mqtt_client, topic, 1);
        ESP_LOGI(TAG, "Subscribed to all command topics");

        /* Upload offline Black Box on reconnect */
        black_box_upload_and_clear(mqtt_publish_raw);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected — Black Box continues logging");
        mqtt_connected = false;
        xEventGroupClearBits(mqtt_evt_grp, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA:
        mqtt_handle_command(event->topic, event->topic_len,
                            event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 *  Command dispatcher
 * ---------------------------------------------------------------- */
void mqtt_handle_command(const char *topic, int topic_len,
                          const char *data, int data_len)
{
    char topic_str[128] = {0};
    char data_str[256]  = {0};
    snprintf(topic_str, sizeof(topic_str), "%.*s", topic_len, topic);
    snprintf(data_str,  sizeof(data_str),  "%.*s", data_len,  data);

    ESP_LOGI(TAG, "CMD: %s → %s", topic_str, data_str);

    cJSON *json = cJSON_ParseWithLength(data_str, data_len);
    if (!json) {
        ESP_LOGW(TAG, "Failed to parse command JSON");
        return;
    }

    /* hub/{id}/cmd/gate */
    if (strstr(topic_str, "cmd/gate")) {
        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (cJSON_IsString(action)) {
            third_wire_set_mode_from_mqtt(action->valuestring);
        }
    }
    /* hub/{id}/cmd/pwm */
    else if (strstr(topic_str, "cmd/pwm")) {
        cJSON *duty = cJSON_GetObjectItem(json, "duty");
        if (cJSON_IsNumber(duty)) {
            uint32_t d = (uint32_t)duty->valuedouble;
            third_wire_set_pwm_duty(d);
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.pwm_duty_pct = d;
            xSemaphoreGive(g_state_mutex);
        }
    }
    /* hub/{id}/cmd/reset_fault */
    else if (strstr(topic_str, "cmd/reset_fault")) {
        cJSON *confirm = cJSON_GetObjectItem(json, "confirm");
        if (cJSON_IsTrue(confirm)) {
            scp_clear_permanent_fault();
        }
    }
    /* hub/{id}/cmd/config */
    else if (strstr(topic_str, "cmd/config")) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        cJSON *ov  = cJSON_GetObjectItem(json, "ov_mv");
        cJSON *uv  = cJSON_GetObjectItem(json, "uv_mv");
        cJSON *oc  = cJSON_GetObjectItem(json, "oc_ma");
        cJSON *bal = cJSON_GetObjectItem(json, "bal_delta_mv");
        if (cJSON_IsNumber(ov))  g_sys.ov_mv        = (uint32_t)ov->valuedouble;
        if (cJSON_IsNumber(uv))  g_sys.uv_mv        = (uint32_t)uv->valuedouble;
        if (cJSON_IsNumber(oc))  g_sys.oc_ma        = (uint32_t)oc->valuedouble;
        if (cJSON_IsNumber(bal)) g_sys.bal_delta_mv = (uint32_t)bal->valuedouble;
        xSemaphoreGive(g_state_mutex);
        config_save_to_nvs();
        ESP_LOGI(TAG, "Config updated: OV=%d UV=%d OC=%d BAL=%d",
                 g_sys.ov_mv, g_sys.uv_mv, g_sys.oc_ma, g_sys.bal_delta_mv);
    }

    cJSON_Delete(json);
}

/* ----------------------------------------------------------------
 *  Publish: hub/{id}/telemetry — every 5 seconds
 *  Exact JSON structure from Section 9.1
 * ---------------------------------------------------------------- */
static void publish_telemetry(void)
{
    if (!mqtt_connected) return;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    system_state_t snap = g_sys;
    xSemaphoreGive(g_state_mutex);

    const char *gate_str =
        snap.gate_state == GATE_FULL_POWER ? "FULL_POWER" :
        snap.gate_state == GATE_PWM        ? "PWM_50"     : "OFF";

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"device_id\":\"%s\","
        "\"timestamp\":%u,"
        "\"pack_voltage_mv\":%u,"
        "\"pack_current_ma\":%d,"
        "\"soc_percent\":%.1f,"
        "\"temp_avg_c\":%.1f,"
        "\"temp_sensors_c\":[%.1f,%.1f,%.1f,%.1f],"
        "\"gate_state\":\"%s\","
        "\"pwm_duty_percent\":%u,"
        "\"fault_active\":%s,"
        "\"retry_count\":%u,"
        "\"wifi_rssi\":%d"
        "}",
        CONFIG_DEVICE_ID,
        (uint32_t)time(NULL),
        snap.pack_voltage_mv,
        snap.pack_current_ma,
        snap.soc_percent_x10 / 10.0f,
        snap.temp_avg_c,
        snap.temp_sensors_c[0], snap.temp_sensors_c[1], snap.temp_sensors_c[2], snap.temp_sensors_c[3],
        gate_str,
        snap.pwm_duty_pct,
        snap.fault_active ? "true" : "false",
        snap.retry_count,
        snap.wifi_rssi
    );

    char topic[64];
    make_topic(topic, sizeof(topic), "telemetry");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

/* ----------------------------------------------------------------
 *  Publish: hub/{id}/cells — every 30 seconds
 *  Exact JSON structure from Section 9.2
 * ---------------------------------------------------------------- */
static void publish_cells(void)
{
    if (!mqtt_connected) return;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint16_t cells[CELL_COUNT];
    bool     balancing[CELL_COUNT];
    memcpy(cells,     g_sys.cell_mv,       sizeof(cells));
    memcpy(balancing, g_sys.cell_balancing, sizeof(balancing));
    uint32_t pack_v = g_sys.pack_voltage_mv;
    xSemaphoreGive(g_state_mutex);

    /* Find min/max/delta */
    uint16_t min_mv = cells[0], max_mv = cells[0];
    for (int i = 1; i < CELL_COUNT; i++) {
        if (cells[i] < min_mv) min_mv = cells[i];
        if (cells[i] > max_mv) max_mv = cells[i];
    }

    /* Build cells_mv array string */
    char cells_str[256] = "[";
    for (int i = 0; i < CELL_COUNT; i++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%u%s", cells[i], i < CELL_COUNT-1 ? "," : "]");
        strncat(cells_str, tmp, sizeof(cells_str) - strlen(cells_str) - 1);
    }

    /* Build balancing array string */
    char bal_str[128] = "[";
    for (int i = 0; i < CELL_COUNT; i++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%s%s",
                 balancing[i] ? "true" : "false",
                 i < CELL_COUNT-1 ? "," : "]");
        strncat(bal_str, tmp, sizeof(bal_str) - strlen(bal_str) - 1);
    }

    char payload[600];
    snprintf(payload, sizeof(payload),
        "{"
        "\"device_id\":\"%s\","
        "\"timestamp\":%u,"
        "\"cells_mv\":%s,"
        "\"cell_min_mv\":%u,"
        "\"cell_max_mv\":%u,"
        "\"cell_delta_mv\":%u,"
        "\"balancing\":%s"
        "}",
        CONFIG_DEVICE_ID,
        (uint32_t)time(NULL),
        cells_str,
        min_mv, max_mv,
        (uint16_t)(max_mv - min_mv),
        bal_str
    );

    char topic[64];
    make_topic(topic, sizeof(topic), "cells");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

/* ----------------------------------------------------------------
 *  Publish: hub/{id}/faults — on-event
 * ---------------------------------------------------------------- */
void mqtt_publish_fault(const char *fault_type, int32_t peak_current)
{
    if (!mqtt_connected) return;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%u,"
        "\"fault_type\":\"%s\",\"peak_current_ma\":%d,"
        "\"pack_voltage_mv\":%u,\"retry_count\":%u}",
        CONFIG_DEVICE_ID,
        (uint32_t)time(NULL),
        fault_type,
        peak_current,
        g_sys.pack_voltage_mv,
        g_sys.retry_count
    );

    char topic[64];
    make_topic(topic, sizeof(topic), "faults");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

void mqtt_publish_recovery_eta(uint32_t eta_seconds)
{
    if (!mqtt_connected) return;
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"recovery_eta_s\":%u}",
             CONFIG_DEVICE_ID, eta_seconds);
    char topic[64];
    make_topic(topic, sizeof(topic), "faults");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 0, 0);
}

void mqtt_publish_gate_state_change(op_mode_t mode)
{
    if (!mqtt_connected) return;
    const char *s = mode == MODE_FULL_POWER ? "FULL_POWER" :
                    mode == MODE_PWM_50     ? "PWM_50"     : "OFF";
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"gate_state\":\"%s\",\"timestamp\":%u}",
             CONFIG_DEVICE_ID, s, (uint32_t)time(NULL));
    char topic[64];
    make_topic(topic, sizeof(topic), "telemetry");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 0, 0);
}

/* ----------------------------------------------------------------
 *  Raw publish (used by black_box upload)
 * ---------------------------------------------------------------- */
void mqtt_publish_raw(const char *topic, const char *payload)
{
    if (!mqtt_connected || !mqtt_client) return;
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 0, 0);
}

/* ----------------------------------------------------------------
 *  MQTT client init
 * ---------------------------------------------------------------- */
static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.client_id = CONFIG_DEVICE_ID,
        /* Add .credentials.username / .password from NVS in production */
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* ----------------------------------------------------------------
 *  Main MQTT telemetry task
 * ---------------------------------------------------------------- */
void mqtt_telemetry_task(void *arg)
{
    mqtt_evt_grp = xEventGroupCreate();
    mqtt_start();

    uint32_t telemetry_ticks = 0;
    uint32_t cells_ticks     = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));   // 1s loop tick

        if (!mqtt_connected) continue;

        telemetry_ticks += 1000;
        cells_ticks     += 1000;

        if (telemetry_ticks >= TELEMETRY_INTERVAL_MS) {
            publish_telemetry();
            telemetry_ticks = 0;
        }

        if (cells_ticks >= CELL_REPORT_INTERVAL_MS) {
            publish_cells();
            cells_ticks = 0;
        }
    }
}

/* Stub for fault publish task when in safe standby */
void mqtt_fault_task(void *arg)
{
    mqtt_evt_grp = xEventGroupCreate();
    mqtt_start();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        /* Only publishes when fault was queued — handled by event handler */
    }
}
