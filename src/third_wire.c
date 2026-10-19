/**
 * third_wire.c — Third-Wire interlock and centralized gate/PWM control
 */

#include "third_wire.h"
#include "pwm_mode.h"
#include "config.h"
#include "hardware_init.h"
#include "black_box.h"
#include "mqtt_telemetry.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "THIRD_WIRE";

#define NVS_NAMESPACE "24shub"
#define NVS_KEY_MODE  "op_mode"
#define NVS_KEY_DUTY  "pwm_duty"

static void save_mode_to_nvs(op_mode_t mode, uint32_t duty)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
        nvs_set_u32(h, NVS_KEY_DUTY, duty);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_mode_from_nvs(op_mode_t *mode, uint32_t *duty)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *mode = MODE_PWM;
        *duty = 50U;
        ESP_LOGI(TAG, "No saved output request; defaulting to PWM 50%%");
        return;
    }
    if (err != ESP_OK) {
        *mode = MODE_OFF;
        *duty = 0U;
        ESP_LOGE(TAG, "Unable to open saved output request: %s", esp_err_to_name(err));
        return;
    }

    uint8_t saved_mode = 0U;
    uint32_t saved_duty = 0U;
    esp_err_t mode_err = nvs_get_u8(h, NVS_KEY_MODE, &saved_mode);
    esp_err_t duty_err = nvs_get_u32(h, NVS_KEY_DUTY, &saved_duty);
    nvs_close(h);

    if (mode_err == ESP_ERR_NVS_NOT_FOUND && duty_err == ESP_ERR_NVS_NOT_FOUND) {
        *mode = MODE_PWM;
        *duty = 50U;
        ESP_LOGI(TAG, "No saved output request; defaulting to PWM 50%%");
        return;
    }
    if (mode_err != ESP_OK || duty_err != ESP_OK ||
        saved_mode > MODE_PWM || saved_duty > 100U) {
        *mode = MODE_OFF;
        *duty = 0U;
        ESP_LOGW(TAG, "Invalid saved output request; defaulting to OFF");
        return;
    }

    *mode = (op_mode_t)saved_mode;
    *duty = saved_duty;
    ESP_LOGI(TAG, "Restored saved output request: mode %u, duty %u%%",
             (unsigned)saved_mode, (unsigned)saved_duty);
}

static bool output_inhibited_locked(bool recovery_authorized)
{
    uint32_t faults = g_sys.protection_fault_mask;
    bool recovery_state = g_sys.scp_state == SCP_STATE_RETRY_PROBE ||
                          g_sys.scp_state == SCP_STATE_GATE_RESTORE;
    return g_sys.permanent_fault ||
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

static void resolve_master_mode(op_mode_t *mode, uint32_t *duty_pct)
{
    if (MASTER_SWITCH_GPIO < 0 || *mode == MODE_OFF) {
        return;
    }

    int level = gpio_get_level(MASTER_SWITCH_GPIO);
    if (level < 0) {
        *mode = MODE_OFF;
        *duty_pct = 0U;
    } else if ((level == 1) == (MASTER_SWITCH_HIGH_IS_100PCT != 0)) {
        *mode = MODE_FULL_POWER;
        *duty_pct = 100U;
    } else {
        *mode = MODE_PWM;
        *duty_pct = 50U;
    }
}

void third_wire_refresh_output(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_active || output_inhibited_locked(false)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return;
    }
    op_mode_t mode = g_sys.op_mode;
    uint32_t duty = g_sys.pwm_duty_pct;
    resolve_master_mode(&mode, &duty);
    drive_output_locked(mode, duty);
    xSemaphoreGive(g_state_mutex);
}

static void apply_mode(op_mode_t mode, uint32_t duty_pct)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (output_inhibited_locked(false)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        ESP_LOGW(TAG, "Output inhibited by fault state — mode change blocked");
        return;
    }
    g_sys.op_mode = mode;
    g_sys.pwm_duty_pct = duty_pct;
    xSemaphoreGive(g_state_mutex);

    third_wire_refresh_output();
    if (mode == MODE_PWM) {
        ESP_LOGI(TAG, "Mode request: PWM %u%%", (unsigned)duty_pct);
    } else {
        ESP_LOGI(TAG, "Mode request: %s",
                 mode == MODE_OFF ? "OFF" : "FULL POWER");
    }

    black_box_write_gate_change(mode);
    save_mode_to_nvs(mode, duty_pct);
}

void third_wire_task(void *arg)
{
    (void)arg;
    op_mode_t mode;
    uint32_t duty;
    load_mode_from_nvs(&mode, &duty);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.op_mode = mode;
    g_sys.pwm_duty_pct = duty;
    g_sys.pwm_remote_override = true;
    g_sys.third_wire_active = false;
    xSemaphoreGive(g_state_mutex);

    ESP_LOGI(TAG, "Gate control task running, boot request mode: %d", mode);

    bool stable_active = false;
    bool candidate_active = false;
    int64_t candidate_since_us = esp_timer_get_time();
    int master_stable = (MASTER_SWITCH_GPIO >= 0) ?
                        gpio_get_level(MASTER_SWITCH_GPIO) : -1;
    int master_candidate = master_stable;
    int64_t master_candidate_since_us = candidate_since_us;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POT_POLL_INTERVAL_MS));

        int input_level = gpio_get_level(PIN_THIRD_WIRE);
        bool raw_active = input_level == 1;
        int64_t now_us = esp_timer_get_time();
        if (input_level < 0) {
            raw_active = false;
        }
        if (raw_active != candidate_active) {
            candidate_active = raw_active;
            candidate_since_us = now_us;
        } else if (candidate_active != stable_active &&
                   now_us - candidate_since_us >=
                       (int64_t)THIRD_WIRE_DEBOUNCE_MS * 1000LL) {
            stable_active = candidate_active;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.third_wire_active = stable_active;
            xSemaphoreGive(g_state_mutex);
            ESP_LOGI(TAG, "Third-Wire input %s",
                     stable_active ? "active" : "inactive");
            third_wire_refresh_output();
        }

        if (MASTER_SWITCH_GPIO >= 0) {
            int raw_master = gpio_get_level(MASTER_SWITCH_GPIO);
            if (raw_master < 0) raw_master = 0;
            if (raw_master != master_candidate) {
                master_candidate = raw_master;
                master_candidate_since_us = now_us;
            } else if (master_candidate != master_stable &&
                       now_us - master_candidate_since_us >=
                           (int64_t)THIRD_WIRE_DEBOUNCE_MS * 1000LL) {
                master_stable = master_candidate;
                third_wire_refresh_output();
            }
        }

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        bool apply_pot = g_sys.boot_complete &&
                         g_sys.third_wire_active &&
                         !g_sys.fault_active &&
                         !g_sys.pwm_remote_override &&
                         g_sys.op_mode == MODE_PWM &&
                         MASTER_SWITCH_GPIO < 0;
        xSemaphoreGive(g_state_mutex);
        if (apply_pot) {
            pwm_apply_pot();
        }
    }
}

bool third_wire_apply_recovery_probe(uint32_t duty_pct)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_active || output_inhibited_locked(true)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return false;
    }
    drive_output_locked(MODE_PWM, duty_pct);
    xSemaphoreGive(g_state_mutex);
    return true;
}

void third_wire_apply_recovery_mode(op_mode_t mode, uint32_t duty_pct)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    if (!g_sys.third_wire_active || output_inhibited_locked(true)) {
        force_output_off_locked();
        xSemaphoreGive(g_state_mutex);
        return;
    }
    resolve_master_mode(&mode, &duty_pct);
    drive_output_locked(mode, duty_pct);
    xSemaphoreGive(g_state_mutex);
}

void third_wire_set_mode_from_mqtt(const char *action)
{
    op_mode_t mode = MODE_OFF;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t duty = g_sys.pwm_duty_pct;
    xSemaphoreGive(g_state_mutex);

    if (strcmp(action, "ON") == 0) {
        mode = MODE_FULL_POWER;
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.pwm_remote_override = false;
        xSemaphoreGive(g_state_mutex);
    } else if (strcmp(action, "OFF") == 0) {
        mode = MODE_OFF;
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.pwm_remote_override = false;
        xSemaphoreGive(g_state_mutex);
    } else if (strcmp(action, "PWM") == 0) {
        mode = MODE_PWM;
        if (duty == 0U) duty = 50U;
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.pwm_remote_override = true;
        xSemaphoreGive(g_state_mutex);
    }

    apply_mode(mode, duty);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    op_mode_t published_mode = g_sys.op_mode;
    xSemaphoreGive(g_state_mutex);
    mqtt_publish_gate_state_change(published_mode);
}

void third_wire_set_pwm_duty(uint32_t duty_pct)
{
    if (duty_pct > 100U) duty_pct = 100U;

    /* Preserve the PWA-facing mode semantics as PWM even when the hardware output is effectively full-scale,
     * while still driving the gate with the requested duty. */
    op_mode_t mode = (duty_pct == 0U) ? MODE_OFF : MODE_PWM;

    apply_mode(mode, duty_pct);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.pwm_remote_override = true;
    xSemaphoreGive(g_state_mutex);

    save_mode_to_nvs(mode, duty_pct);
    ESP_LOGI(TAG, "PWM duty updated to %d%% (mode %d)", duty_pct, mode);
}
