/**
 * third_wire.c — MQTT-driven gate/PWM control path
 *
 * The legacy GPIO5 physical third-wire input has been removed from the
 * reconciled PCB. The control interface now flows through GPIO4, with the
 * PWA/MQTT commands driving the operating mode and PWM duty.
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
    /* Safe boot policy: do not restore a prior active output state.
     * NVS data is intentionally ignored until a valid command is received.
     */
    (void)mode;
    (void)duty;
    *mode = MODE_OFF;
    *duty = 0U;
    ESP_LOGI(TAG, "Boot default restored to OFF: duty %d%%", *duty);
}

static bool output_inhibited(void)
{
    return g_sys.permanent_fault || g_sys.fault_active;
}

static void apply_mode(op_mode_t mode, uint32_t duty_pct)
{
    if (output_inhibited()) {
        ESP_LOGW(TAG, "Output inhibited by fault state — mode change blocked");
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 0);
        return;
    }

    switch (mode) {
    case MODE_OFF:
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 0);
        ESP_LOGI(TAG, "Mode → OFF");
        break;

    case MODE_FULL_POWER:
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 1);
        ESP_LOGI(TAG, "Mode → FULL POWER");
        break;

    case MODE_PWM_50:
        pwm_set_duty(duty_pct);
        ESP_LOGI(TAG, "Mode → PWM %d%%", duty_pct);
        break;
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.op_mode = mode;
    g_sys.pwm_duty_pct = duty_pct;
    g_sys.gate_state = (mode == MODE_OFF) ? GATE_OFF :
                       (mode == MODE_FULL_POWER) ? GATE_FULL_POWER : GATE_PWM;
    xSemaphoreGive(g_state_mutex);

    black_box_write_gate_change(mode);
    save_mode_to_nvs(mode, duty_pct);
}

void third_wire_task(void *arg)
{
    op_mode_t mode;
    uint32_t duty;
    load_mode_from_nvs(&mode, &duty);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.op_mode = MODE_OFF;
    g_sys.pwm_duty_pct = 0U;
    xSemaphoreGive(g_state_mutex);

    if (g_sys.boot_complete) {
        apply_mode(MODE_OFF, 0U);
    }

    ESP_LOGI(TAG, "Gate control task running, current mode: %d", mode);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!g_sys.boot_complete || output_inhibited()) {
            continue;
        }
        if (g_sys.op_mode == MODE_PWM_50 && !g_sys.pwm_remote_override) {
            pwm_apply_pot();
        }
    }
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
        mode = MODE_PWM_50;
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

    /* Preserve the PWA-facing mode semantics as PWM_50 even when the hardware output is effectively full-scale,
     * while still driving the gate with the requested duty. */
    op_mode_t mode = (duty_pct == 0U) ? MODE_OFF : MODE_PWM_50;

    if (mode == MODE_OFF) {
        apply_mode(MODE_OFF, 0U);
    } else {
        pwm_set_duty(duty_pct);
        apply_mode(MODE_PWM_50, duty_pct);
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.pwm_remote_override = true;
    xSemaphoreGive(g_state_mutex);

    save_mode_to_nvs(mode, duty_pct);
    ESP_LOGI(TAG, "PWM duty updated to %d%% (mode %d)", duty_pct, mode);
}
