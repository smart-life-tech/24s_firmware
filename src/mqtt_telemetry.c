/**
 * mqtt_telemetry.c — Bidirectional MQTT JSON Telemetry
 *
 * Existing PWA-compatible topics are retained. Event payloads use one stable
 * fault schema, while telemetry remains the full state snapshot.
 */

#include "mqtt_telemetry.h"
#include "config.h"
#include "third_wire.h"
#include "scp_recovery.h"
#include "black_box.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

static const char *TAG = "MQTT";

#ifndef CONFIG_DEVICE_ID
#define CONFIG_DEVICE_ID "24S-HUB-001"
#endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtt://10.208.47.228:1883"
#endif

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static EventGroupHandle_t mqtt_evt_grp = NULL;
#define MQTT_CONNECTED_BIT BIT0

static void publish_telemetry(void);

static void make_topic(char *buf, size_t len, const char *suffix)
{
    snprintf(buf, len, "hub/%s/%s", CONFIG_DEVICE_ID, suffix);
}

static const char *chemistry_name(cell_chemistry_t chemistry)
{
    return chemistry == CHEM_SODIUM_ION ? "NA_ION" : "LIFEPO4";
}

static bool valid_config_ranges(uint32_t ov, uint32_t uv, uint32_t oc, uint32_t bal)
{
    if (ov < 2000U || ov > 5000U) return false;
    if (uv < 1500U || uv > 4000U) return false;
    if (uv >= ov) return false;
    if (oc < 1000U || oc > 100000U) return false;
    if (bal < 5U || bal > 500U) return false;
    return true;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        xEventGroupSetBits(mqtt_evt_grp, MQTT_CONNECTED_BIT);
        ESP_LOGI(TAG, "MQTT connected");

        {
            char topic[64];
            make_topic(topic, sizeof(topic), "cmd/gate");
            esp_mqtt_client_subscribe(mqtt_client, topic, 1);
            make_topic(topic, sizeof(topic), "cmd/pwm");
            esp_mqtt_client_subscribe(mqtt_client, topic, 1);
            make_topic(topic, sizeof(topic), "cmd/reset_fault");
            esp_mqtt_client_subscribe(mqtt_client, topic, 1);
            make_topic(topic, sizeof(topic), "cmd/config");
            esp_mqtt_client_subscribe(mqtt_client, topic, 1);
        }

        /* Queue Black Box upload; actual upload runs outside the MQTT callback. */
        black_box_upload_and_clear(mqtt_publish_raw);
        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        xEventGroupClearBits(mqtt_evt_grp, MQTT_CONNECTED_BIT);
        ESP_LOGW(TAG, "MQTT disconnected; Black Box remains buffered");
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

void mqtt_handle_command(const char *topic, int topic_len,
                         const char *data, int data_len)
{
    char topic_str[128] = {0};
    char data_str[256] = {0};
    snprintf(topic_str, sizeof(topic_str), "%.*s", topic_len, topic);
    snprintf(data_str, sizeof(data_str), "%.*s", data_len, data);

    cJSON *json = cJSON_ParseWithLength(data_str, (size_t)data_len);
    if (!json) {
        ESP_LOGW(TAG, "Invalid command JSON");
        return;
    }

    if (strstr(topic_str, "cmd/gate")) {
        cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
        if (cJSON_IsString(action) &&
            (!strcmp(action->valuestring, "ON") ||
             !strcmp(action->valuestring, "OFF") ||
             !strcmp(action->valuestring, "PWM"))) {
            third_wire_set_mode_from_mqtt(action->valuestring);
        }
    } else if (strstr(topic_str, "cmd/pwm")) {
        cJSON *duty = cJSON_GetObjectItemCaseSensitive(json, "duty");
        if (cJSON_IsNumber(duty) && isfinite(duty->valuedouble) &&
            duty->valuedouble >= 0.0 && duty->valuedouble <= 100.0) {
            third_wire_set_pwm_duty((uint32_t)duty->valuedouble);
        } else {
            ESP_LOGW(TAG, "Rejected invalid PWM duty");
        }
    } else if (strstr(topic_str, "cmd/reset_fault")) {
        cJSON *confirm = cJSON_GetObjectItemCaseSensitive(json, "confirm");
        if (cJSON_IsTrue(confirm)) {
            scp_clear_permanent_fault();
        }
    } else if (strstr(topic_str, "cmd/config")) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);

        uint32_t ov = g_sys.ov_mv;
        uint32_t uv = g_sys.uv_mv;
        uint32_t oc = g_sys.oc_ma;
        uint32_t bal = g_sys.bal_delta_mv;
        cell_chemistry_t chemistry = g_sys.chemistry;

        cJSON *ov_j = cJSON_GetObjectItemCaseSensitive(json, "ov_mv");
        cJSON *uv_j = cJSON_GetObjectItemCaseSensitive(json, "uv_mv");
        cJSON *oc_j = cJSON_GetObjectItemCaseSensitive(json, "oc_ma");
        cJSON *bal_j = cJSON_GetObjectItemCaseSensitive(json, "bal_delta_mv");
        cJSON *chem_j = cJSON_GetObjectItemCaseSensitive(json, "chemistry");

        bool ov_supplied = cJSON_IsNumber(ov_j);
        bool uv_supplied = cJSON_IsNumber(uv_j);

        if (ov_supplied && isfinite(ov_j->valuedouble) && ov_j->valuedouble >= 0.0) ov = (uint32_t)ov_j->valuedouble;
        if (uv_supplied && isfinite(uv_j->valuedouble) && uv_j->valuedouble >= 0.0) uv = (uint32_t)uv_j->valuedouble;
        if (cJSON_IsNumber(oc_j) && isfinite(oc_j->valuedouble) && oc_j->valuedouble >= 0.0) oc = (uint32_t)oc_j->valuedouble;
        if (cJSON_IsNumber(bal_j) && isfinite(bal_j->valuedouble) && bal_j->valuedouble >= 0.0) bal = (uint32_t)bal_j->valuedouble;

        if (cJSON_IsString(chem_j)) {
            const char *c = chem_j->valuestring;
            if (!strcasecmp(c, "NA") || !strcasecmp(c, "NA_ION") || !strcasecmp(c, "SODIUM")) {
                chemistry = CHEM_SODIUM_ION;
            } else if (!strcasecmp(c, "LFP") || !strcasecmp(c, "LIFEPO4")) {
                chemistry = CHEM_LIFEPO4;
            } else {
                ESP_LOGW(TAG, "Rejected unknown chemistry '%s'", c);
            }
        }

        /* Chemistry changes establish safe defaults only when OV/UV were not
         * explicitly supplied in the same command. This preserves PWA custom
         * thresholds rather than silently overwriting them on every scan. */
        if (chemistry != g_sys.chemistry) {
            if (!ov_supplied) ov = (chemistry == CHEM_SODIUM_ION) ? OV_MV_NA_ION : OV_MV_LIFEPO4;
            if (!uv_supplied) uv = (chemistry == CHEM_SODIUM_ION) ? UV_MV_NA_ION : UV_MV_LIFEPO4;
        }

        bool valid = valid_config_ranges(ov, uv, oc, bal);
        if (valid) {
            g_sys.ov_mv = ov;
            g_sys.uv_mv = uv;
            g_sys.oc_ma = oc;
            g_sys.bal_delta_mv = bal;
            g_sys.chemistry = chemistry;
        }
        xSemaphoreGive(g_state_mutex);

        if (valid) {
            config_save_to_nvs();
            ESP_LOGI(TAG, "Config updated: OV=%u UV=%u OC=%u BAL=%u CHEM=%s",
                     ov, uv, oc, bal, chemistry_name(chemistry));
        } else {
            ESP_LOGW(TAG, "Rejected invalid protection configuration");
        }
    }

    cJSON_Delete(json);
}

static void publish_telemetry(void)
{
    if (!mqtt_connected || !mqtt_client) return;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    system_state_t snap = g_sys;
    xSemaphoreGive(g_state_mutex);

    uint32_t chemistry_scale_mv =
        (snap.chemistry == CHEM_SODIUM_ION) ?
        (OV_MV_NA_ION * CELL_COUNT) :
        (OV_MV_LIFEPO4 * CELL_COUNT);
    uint32_t soc_10 = 0U;
    if (chemistry_scale_mv > 0U) {
        soc_10 = (snap.pack_voltage_mv * 1000U) / chemistry_scale_mv;
        if (soc_10 > 1000U) soc_10 = 1000U;
    }

    const char *gate_str =
        snap.gate_state == GATE_FULL_POWER ? "FULL_POWER" :
        snap.gate_state == GATE_PWM ? "PWM_50" : "OFF";

    char payload[640];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,"
        "\"pack_voltage_mv\":%lu,\"pack_current_ma\":%ld,"
        "\"soc_percent\":%.1f,\"temp_avg_c\":%.1f,"
        "\"temp_sensors_c\":[%.1f,%.1f,%.1f,%.1f],"
        "\"gate_state\":\"%s\",\"pwm_duty_percent\":%lu,"
        "\"fault_active\":%s,\"retry_count\":%lu,\"wifi_rssi\":%d,"
        "\"chemistry\":\"%s\"}",
        CONFIG_DEVICE_ID, (unsigned long)time(NULL),
        (unsigned long)snap.pack_voltage_mv, (long)snap.pack_current_ma,
        soc_10 / 10.0f, snap.temp_avg_c,
        snap.temp_sensors_c[0], snap.temp_sensors_c[1],
        snap.temp_sensors_c[2], snap.temp_sensors_c[3],
        gate_str, (unsigned long)snap.pwm_duty_pct,
        snap.fault_active ? "true" : "false",
        (unsigned long)snap.retry_count, snap.wifi_rssi,
        chemistry_name(snap.chemistry));

    char topic[64];
    make_topic(topic, sizeof(topic), "telemetry");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

static void publish_cells(void)
{
    if (!mqtt_connected || !mqtt_client) return;

    uint16_t cells[CELL_COUNT];
    bool balancing[CELL_COUNT];
    uint32_t pack_v;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    memcpy(cells, g_sys.cell_mv, sizeof(cells));
    memcpy(balancing, g_sys.cell_balancing, sizeof(balancing));
    pack_v = g_sys.pack_voltage_mv;
    xSemaphoreGive(g_state_mutex);

    (void)pack_v;
    uint16_t min_mv = cells[0], max_mv = cells[0];
    for (int i = 1; i < CELL_COUNT; i++) {
        if (cells[i] < min_mv) min_mv = cells[i];
        if (cells[i] > max_mv) max_mv = cells[i];
    }

    char cells_str[256] = "[";
    for (int i = 0; i < CELL_COUNT; i++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%u%s", cells[i], i < CELL_COUNT - 1 ? "," : "]");
        strncat(cells_str, tmp, sizeof(cells_str) - strlen(cells_str) - 1U);
    }

    char bal_str[128] = "[";
    for (int i = 0; i < CELL_COUNT; i++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%s%s", balancing[i] ? "true" : "false",
                 i < CELL_COUNT - 1 ? "," : "]");
        strncat(bal_str, tmp, sizeof(bal_str) - strlen(bal_str) - 1U);
    }

    char payload[600];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,\"cells_mv\":%s,"
        "\"cell_min_mv\":%u,\"cell_max_mv\":%u,\"cell_delta_mv\":%u,"
        "\"balancing\":%s}",
        CONFIG_DEVICE_ID, (unsigned long)time(NULL), cells_str,
        min_mv, max_mv, (uint16_t)(max_mv - min_mv), bal_str);

    char topic[64];
    make_topic(topic, sizeof(topic), "cells");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

void mqtt_publish_fault(const char *fault_type, int32_t peak_current)
{
    if (!mqtt_connected || !mqtt_client) return;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t pack_v = g_sys.pack_voltage_mv;
    uint32_t retry = g_sys.retry_count;
    xSemaphoreGive(g_state_mutex);

    char payload[320];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,"
        "\"fault_type\":\"%s\",\"peak_current_ma\":%ld,"
        "\"pack_voltage_mv\":%lu,\"retry_count\":%lu}",
        CONFIG_DEVICE_ID, (unsigned long)time(NULL), fault_type,
        (long)peak_current, (unsigned long)pack_v, (unsigned long)retry);

    char topic[64];
    make_topic(topic, sizeof(topic), "faults");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

void mqtt_publish_recovery_eta(uint32_t eta_seconds)
{
    if (!mqtt_connected || !mqtt_client) return;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t pack_v = g_sys.pack_voltage_mv;
    uint32_t retry = g_sys.retry_count;
    xSemaphoreGive(g_state_mutex);

    char payload[320];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,"
        "\"fault_type\":\"SCP_RECOVERY\",\"peak_current_ma\":0,"
        "\"pack_voltage_mv\":%lu,\"retry_count\":%lu,\"recovery_eta_s\":%lu}",
        CONFIG_DEVICE_ID, (unsigned long)time(NULL), (unsigned long)pack_v,
        (unsigned long)retry, (unsigned long)eta_seconds);

    char topic[64];
    make_topic(topic, sizeof(topic), "faults");
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

void mqtt_publish_gate_state_change(op_mode_t mode)
{
    (void)mode;
    /* Publish the normal full telemetry shape rather than a second JSON schema
     * on hub/{id}/telemetry. */
    publish_telemetry();
}

bool mqtt_publish_raw(const char *topic, const char *payload)
{
    if (!mqtt_connected || !mqtt_client || !topic || !payload) return false;
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
    return msg_id >= 0;
}

static esp_err_t mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.client_id = CONFIG_DEVICE_ID,
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    if (!mqtt_client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) return err;
    return esp_mqtt_client_start(mqtt_client);
}

void mqtt_telemetry_task(void *arg)
{
    (void)arg;
    mqtt_evt_grp = xEventGroupCreate();
    if (!mqtt_evt_grp) {
        ESP_LOGE(TAG, "MQTT event group creation failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = mqtt_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    uint32_t telemetry_ticks = 0U;
    uint32_t cells_ticks = 0U;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!mqtt_connected) continue;

        telemetry_ticks += 1000U;
        cells_ticks += 1000U;
        if (telemetry_ticks >= TELEMETRY_INTERVAL_MS) {
            publish_telemetry();
            telemetry_ticks = 0U;
        }
        if (cells_ticks >= CELL_REPORT_INTERVAL_MS) {
            publish_cells();
            cells_ticks = 0U;
        }
    }
}

void mqtt_fault_task(void *arg)
{
    (void)arg;
    mqtt_evt_grp = xEventGroupCreate();
    if (!mqtt_evt_grp) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = mqtt_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Safe-standby MQTT start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
