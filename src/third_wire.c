/**
 * third_wire.c — Third-Wire trigger input and centralized gate control
 */

#include "third_wire.h"
#include "pwm_mode.h"
#include "config.h"
#include "hardware_init.h"
#include "black_box.h"
#include "mqtt_telemetry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "THIRD_WIRE";

static bool output_inhibited_locked(bool recovery_authorized)
{
    uint32_t faults = g_sys.protection_fault_mask;
    bool recovery_state = g_sys.scp_state == SCP_STATE_RETRY_PROBE ||
                          g_sys.scp_state == SCP_STATE_GATE_RESTORE;
    return !g_sys.output_enable_requested ||
           !g_sys.third_wire_valid ||
           g_sys.permanent_fault ||
           (!recovery_authorized && g_sys.fault_active) ||
           (recovery_authorized &&
            (!recovery_state || (faults & PROT_FAULT_SCP) == 0U ||
             (faults & ~PROT_FAULT_SCP) != 0U));
}

static void force_output_off_locked(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 0);
    g_sys.gate_state = GATE_OFF;
}

static void drive_output_locked(op_mode_t mode, uint32_t duty_pct)
{
    switch (mode) {
    case MODE_OFF:
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 0);
        break;

    case MODE_FULL_POWER:
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 1);
        break;

    case MODE_PWM:
        pwm_set_duty(duty_pct);
        break;
    }

    g_sys.gate_state = (mode == MODE_OFF || (mode == MODE_PWM && duty_pct == 0U)) ? GATE_OFF :
                       (mode == MODE_FULL_POWER) ? GATE_FULL_POWER : GATE_PWM;
}

void third_wire_refresh_output(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_valid) {
        g_sys.op_mode = MODE_OFF;
        g_sys.pwm_duty_pct = 0U;
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return;
    }

    const bool output_requested = g_sys.output_enable_requested;
    const op_mode_t mode = output_requested ? MODE_FULL_POWER : MODE_OFF;
    const uint32_t duty = output_requested ? 100U : 0U;
    g_sys.op_mode = mode;
    g_sys.pwm_duty_pct = duty;
    if (output_inhibited_locked(false)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return;
    }
    drive_output_locked(mode, duty);
    xSemaphoreGive(g_state_mutex);
}

static void set_output_enabled(bool enabled)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (enabled && (g_sys.fault_active || g_sys.protection_fault_mask != 0U ||
                    g_sys.permanent_fault)) {
        g_sys.output_enable_requested = false;
        xSemaphoreGive(g_state_mutex);
        ESP_LOGW(TAG, "Output enable rejected while fault is active");
        third_wire_refresh_output();
        return;
    }
    g_sys.output_enable_requested = enabled;
    op_mode_t selected_mode = enabled ? MODE_FULL_POWER : MODE_OFF;
    xSemaphoreGive(g_state_mutex);

    third_wire_refresh_output();
    ESP_LOGI(TAG, "Remote output request: %s", enabled ? "ON" : "OFF");
    black_box_write_gate_change(selected_mode);
}

void third_wire_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.output_enable_requested = false;
    g_sys.third_wire_high = false;
    g_sys.third_wire_valid = false;
    g_sys.op_mode = MODE_OFF;
    g_sys.pwm_duty_pct = 0U;
    force_output_off_locked();
    xSemaphoreGive(g_state_mutex);

    ESP_LOGI(TAG, "Gate control task running; output defaults OFF");

    bool candidate_high = false;
    bool candidate_valid = false;
    bool input_error_logged = false;
    int64_t candidate_since_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POT_POLL_INTERVAL_MS));

        int input_level = gpio_get_level(PIN_THIRD_WIRE);
        int64_t now_us = esp_timer_get_time();
        if (input_level < 0) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.third_wire_valid = false;
            g_sys.output_enable_requested = false;
            g_sys.op_mode = MODE_OFF;
            g_sys.pwm_duty_pct = 0U;
            force_output_off_locked();
            xSemaphoreGive(g_state_mutex);
            candidate_valid = false;
            if (!input_error_logged) {
                ESP_LOGE(TAG, "Third-Wire selector read failed; output disabled");
                input_error_logged = true;
            }
            continue;
        }
        input_error_logged = false;
        bool raw_high = input_level == 1;
        if (!candidate_valid || raw_high != candidate_high) {
            candidate_valid = true;
            candidate_high = raw_high;
            candidate_since_us = now_us;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.third_wire_valid = false;
            force_output_off_locked();
            xSemaphoreGive(g_state_mutex);
        } else if (now_us - candidate_since_us >=
                       (int64_t)THIRD_WIRE_DEBOUNCE_MS * 1000LL) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            bool changed = !g_sys.third_wire_valid ||
                           g_sys.third_wire_high != candidate_high;
            g_sys.third_wire_high = candidate_high;
            g_sys.third_wire_valid = true;
            xSemaphoreGive(g_state_mutex);
            if (changed) {
                ESP_LOGI(TAG, "Third-Wire trigger %s; output power remains controlled by enable request",
                         candidate_high ? "asserted" : "cleared");
                third_wire_refresh_output();
                mqtt_publish_gate_state_change(g_sys.op_mode);
            }
        }
    }
}

bool third_wire_apply_recovery_probe(uint32_t duty_pct)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_valid || output_inhibited_locked(true)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return false;
    }
    drive_output_locked(MODE_PWM, duty_pct);
    xSemaphoreGive(g_state_mutex);
    return true;
}

void third_wire_apply_recovery_mode(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_valid || output_inhibited_locked(true)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return;
    }
    const bool output_requested = g_sys.output_enable_requested;
    op_mode_t mode = output_requested ? MODE_FULL_POWER : MODE_OFF;
    uint32_t duty_pct = output_requested ? 100U : 0U;
    g_sys.op_mode = mode;
    g_sys.pwm_duty_pct = duty_pct;
    drive_output_locked(mode, duty_pct);
    xSemaphoreGive(g_state_mutex);
}

void third_wire_set_mode_from_mqtt(const char *action)
{
    if (strcmp(action, "ON") == 0) {
        set_output_enabled(true);
    } else if (strcmp(action, "OFF") == 0) {
        set_output_enabled(false);
    } else if (strcmp(action, "PWM") == 0) {
        set_output_enabled(true);
    } else {
        ESP_LOGW(TAG, "Rejected unsupported gate action");
        return;
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    op_mode_t published_mode = g_sys.op_mode;
    xSemaphoreGive(g_state_mutex);
    mqtt_publish_gate_state_change(published_mode);
}

void third_wire_set_pwm_duty(uint32_t duty_pct)
{
    if (duty_pct > 100U) {
        ESP_LOGW(TAG, "Rejected invalid PWM duty %u%%", (unsigned)duty_pct);
        return;
    }
    ESP_LOGI(TAG, "PWM command %u%% mapped to %s; GPIO47 remains trigger-only",
             (unsigned)duty_pct, duty_pct == 0U ? "OFF" : "ON");
    set_output_enabled(duty_pct != 0U);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    op_mode_t selected_mode = g_sys.op_mode;
    xSemaphoreGive(g_state_mutex);
    mqtt_publish_gate_state_change(selected_mode);
}
