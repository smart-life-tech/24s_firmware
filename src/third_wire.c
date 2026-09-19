/**
 * third_wire.c — Third Wire Activation Logic + PWM Mode Switching
 *
 * Section 4 & 5 of the architecture document.
 *
 * Third Wire input (J9/J11) controls the 72V rail only.
 * Each rising edge advances: OFF → FULL POWER → PWM 50% → OFF
 * 200ms debounce. Mode survives power cycles (stored in NVS).
 *
 * PWM: ESP32-S2 LEDC @ 20kHz, 13-bit resolution.
 * INA240A1D handles PWM rejection natively — no blanking circuit needed.
 * Duty cycle adjustable 0-100% via MQTT hub/{id}/cmd/pwm
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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "THIRD_WIRE";

#define NVS_NAMESPACE   "24shub"
#define NVS_KEY_MODE    "op_mode"
#define NVS_KEY_DUTY    "pwm_duty"

static QueueHandle_t tw_evt_queue;
static int64_t last_edge_us = 0;

/* ----------------------------------------------------------------
 *  ISR — Rising edge on Third Wire GPIO
 * ---------------------------------------------------------------- */
static void IRAM_ATTR third_wire_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    /* Debounce: ignore edges less than 200ms after last */
    if ((now - last_edge_us) < (THIRD_WIRE_DEBOUNCE_MS * 1000LL)) {
        return;
    }
    last_edge_us = now;
    uint8_t evt = 1;
    xQueueSendFromISR(tw_evt_queue, &evt, NULL);
}

/* ----------------------------------------------------------------
 *  NVS persist helpers
 * ---------------------------------------------------------------- */
static void save_mode_to_nvs(op_mode_t mode, uint32_t duty)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h,  NVS_KEY_MODE, (uint8_t)mode);
        nvs_set_u32(h, NVS_KEY_DUTY, duty);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_mode_from_nvs(op_mode_t *mode, uint32_t *duty)
{
    nvs_handle_t h;
    *mode = MODE_OFF;
    *duty = 50;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0; uint32_t d = 50;
        nvs_get_u8(h,  NVS_KEY_MODE, &m);
        nvs_get_u32(h, NVS_KEY_DUTY, &d);
        *mode = (op_mode_t)m;
        *duty = d;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Restored mode %d, duty %d%% from NVS", *mode, *duty);
}

/* ----------------------------------------------------------------
 *  Gate control (mirrors scp_recovery but exposed here for mode changes)
 * ---------------------------------------------------------------- */
static void apply_mode(op_mode_t mode, uint32_t duty_pct)
{
    /* Skip if permanent fault is active */
    if (g_sys.permanent_fault) {
        ESP_LOGW(TAG, "Permanent fault active — mode change blocked");
        return;
    }

    switch (mode) {
    case MODE_OFF:
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 0);
        ESP_LOGI(TAG, "Mode → OFF (0V output)");
        break;

    case MODE_FULL_POWER:
        /* Stop LEDC, drive GPIO HIGH directly for zero switching loss */
        ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
        gpio_set_level(PIN_GATE_CTRL, 1);
        ESP_LOGI(TAG, "Mode → FULL POWER (100%% gate drive)");
        break;

    case MODE_PWM_50:
        /*
         * LEDC peripheral drives GATE_CTRL @ 20kHz.
         * INA240A1D internal PWM rejection filter prevents false OCP trips.
         * No external blanking capacitor required (datasheet Section 7.3).
         */
       // pwm_set_duty(duty_pct);
        pwm_apply_pot(); 
        //ESP_LOGI(TAG, "Mode : PWM pot-controlled @ 20kHz", duty_pct);
        ESP_LOGI(TAG, "Mode : PWM pot-controlled @ 20kHz");
        break;
    }

    /* Update shared state */
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.op_mode      = mode;
    g_sys.pwm_duty_pct = duty_pct;
    g_sys.gate_state   = (mode == MODE_OFF)        ? GATE_OFF :
                         (mode == MODE_FULL_POWER)  ? GATE_FULL_POWER : GATE_PWM;
    xSemaphoreGive(g_state_mutex);

    /* Log gate state change to Black Box */
    black_box_write_gate_change(mode);

    /* Persist so next boot starts in same mode */
    save_mode_to_nvs(mode, duty_pct);
}

/* ----------------------------------------------------------------
 *  Mode cycle: OFF → FULL POWER → PWM 50% → OFF
 * ---------------------------------------------------------------- */
static op_mode_t advance_mode(op_mode_t current)
{
    switch (current) {
    case MODE_OFF:        return MODE_FULL_POWER;
    case MODE_FULL_POWER: return MODE_PWM_50;
    case MODE_PWM_50:     return MODE_OFF;
    default:              return MODE_OFF;
    }
}

/* ----------------------------------------------------------------
 *  Third Wire task
 * ---------------------------------------------------------------- */
void third_wire_task(void *arg)
{
    tw_evt_queue = xQueueCreate(4, sizeof(uint8_t));
    gpio_isr_handler_add(PIN_THIRD_WIRE, third_wire_isr_handler, NULL);

    /* Restore last mode from NVS */
    op_mode_t mode;
    uint32_t  duty;
    load_mode_from_nvs(&mode, &duty);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.op_mode      = mode;
    g_sys.pwm_duty_pct = duty;
    xSemaphoreGive(g_state_mutex);

    /* Apply restored mode only if boot is complete and handshake confirmed */
    if (g_sys.boot_complete && g_sys.handshake_done) {
        apply_mode(mode, duty);
    }

    ESP_LOGI(TAG, "Third Wire task running, current mode: %d", mode);
    
    
    uint8_t evt;
	for (;;) {
    
    if (xQueueReceive(tw_evt_queue, &evt,
                      pdMS_TO_TICKS(POT_POLL_INTERVAL_MS)) == pdTRUE) {

        
        if (!g_sys.boot_complete || !g_sys.handshake_done) {
            ESP_LOGW(TAG, "Third Wire pulse ignored : boot not complete");
            continue;
        }
        if (g_sys.permanent_fault) {
            ESP_LOGW(TAG, "Third Wire pulse ignored : permanent fault active");
            continue;
        }

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        mode = g_sys.op_mode;
        duty = g_sys.pwm_duty_pct;
        xSemaphoreGive(g_state_mutex);

        mode = advance_mode(mode);
        ESP_LOGI(TAG, "Third Wire pulse : new mode: %d", mode);
        apply_mode(mode, duty);

        mqtt_publish_gate_state_change(mode);

    } else {
        
        if (g_sys.boot_complete &&
            g_sys.handshake_done &&
            !g_sys.permanent_fault &&
            g_sys.op_mode == MODE_PWM_50) {

            pwm_apply_pot();
        }
    }
}

}

/* ----------------------------------------------------------------
 *  Called from MQTT command handler: hub/{id}/cmd/gate
 * ---------------------------------------------------------------- */
void third_wire_set_mode_from_mqtt(const char *action)
{
    op_mode_t mode;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    uint32_t duty = g_sys.pwm_duty_pct;
    xSemaphoreGive(g_state_mutex);

    if (strcmp(action, "ON") == 0) {
        mode = MODE_FULL_POWER;
    } else {
        mode = MODE_OFF;
    }
    apply_mode(mode, duty);
}

/* ----------------------------------------------------------------
 *  Called from MQTT command handler: hub/{id}/cmd/pwm  {"duty": N}
 * ---------------------------------------------------------------- */
void third_wire_set_pwm_duty(uint32_t duty_pct)
{
    if (duty_pct > 100) duty_pct = 100;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.pwm_duty_pct = duty_pct;
    op_mode_t mode = g_sys.op_mode;
    xSemaphoreGive(g_state_mutex);

    if (mode == MODE_PWM_50) {
        pwm_set_duty(duty_pct);
        save_mode_to_nvs(mode, duty_pct);
    }
    ESP_LOGI(TAG, "PWM duty updated to %d%%", duty_pct);
}
