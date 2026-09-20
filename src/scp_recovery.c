/**
 * scp_recovery.c — Hardware Short-Circuit Protection + Auto-Recovery
 *
 * Hardware protection remains in the TLV3501 → ISO5451DW path. Firmware observes
 * FAULT_N, logs the event, waits 10 s, applies a 5% probe when appropriate, and
 * restores the prior output mode only when all other protection conditions are safe.
 */

#include "scp_recovery.h"
#include "config.h"
#include "hardware_init.h"
#include "ltc6811.h"
#include "black_box.h"
#include "mqtt_telemetry.h"
#include "ina240.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stddef.h>

static const char *TAG = "SCP";

#define SCP_EVT_FAULT 1U
static QueueHandle_t scp_evt_queue;
static int64_t trip_times_us[SCP_MAX_RETRIES];
static uint32_t trip_count_total = 0U;

static void IRAM_ATTR fault_n_isr_handler(void *arg)
{
    (void)arg;
    uint8_t evt = SCP_EVT_FAULT;
    if (scp_evt_queue) xQueueSendFromISR(scp_evt_queue, &evt, NULL);
}

static void gate_enable_full(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 1);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_FULL_POWER;
    xSemaphoreGive(g_state_mutex);
}

static void gate_enable_pwm(uint32_t duty_pct)
{
    if (duty_pct > 100U) duty_pct = 100U;
    uint32_t duty = (duty_pct * LEDC_DUTY_MAX) / 100UL;
    if (ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty) == ESP_OK &&
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL) == ESP_OK) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.gate_state = (duty_pct == 0U) ? GATE_OFF : GATE_PWM;
        xSemaphoreGive(g_state_mutex);
    }
}

static void gate_disable(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 0);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_OFF;
    xSemaphoreGive(g_state_mutex);
}

static bool trip_window_reached(void)
{
    int64_t now = esp_timer_get_time();
    trip_times_us[trip_count_total % SCP_MAX_RETRIES] = now;
    trip_count_total++;

    if (trip_count_total < SCP_MAX_RETRIES) return false;
    uint32_t oldest_index = (trip_count_total - SCP_MAX_RETRIES) % SCP_MAX_RETRIES;
    return (now - trip_times_us[oldest_index]) <= ((int64_t)SCP_RETRY_WINDOW_MS * 1000LL);
}

static void led_blink_task_red(void *arg)
{
    (void)arg;
    if (PIN_STATUS_LED == GPIO_NUM_NC) {
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        gpio_set_level(PIN_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wait_recovery_interval(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.scp_state = SCP_STATE_RECOVERY_WAIT;
    xSemaphoreGive(g_state_mutex);

    mqtt_publish_recovery_eta(SCP_RECOVERY_WAIT_MS / 1000U);
    TaskHandle_t led_task = NULL;
    if (xTaskCreate(led_blink_task_red, "led_red", 1024, NULL, 1, &led_task) != pdPASS) {
        led_task = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(SCP_RECOVERY_WAIT_MS));
    if (led_task) vTaskDelete(led_task);
    if (PIN_STATUS_LED != GPIO_NUM_NC) gpio_set_level(PIN_STATUS_LED, 0);
}

void scp_recovery_task(void *arg)
{
    (void)arg;
    scp_evt_queue = xQueueCreate(4, sizeof(uint8_t));
    if (!scp_evt_queue) {
        ESP_LOGE(TAG, "SCP event queue creation failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = gpio_isr_handler_add(PIN_FAULT_N, fault_n_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAULT_N ISR registration failed: %s", esp_err_to_name(err));
    }

    if (gpio_get_level(PIN_FAULT_N) == 0) {
        uint8_t evt = SCP_EVT_FAULT;
        xQueueSend(scp_evt_queue, &evt, 0);
        ESP_LOGW(TAG, "FAULT_N already low at startup");
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.scp_state = SCP_STATE_NORMAL;
    xSemaphoreGive(g_state_mutex);

    for (;;) {
        uint8_t evt = 0U;
        if (xQueueReceive(scp_evt_queue, &evt, pdMS_TO_TICKS(INA240_POLL_INTERVAL_MS)) != pdTRUE) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.pack_current_ma = ina240_read_current_ma();
            xSemaphoreGive(g_state_mutex);
            continue;
        }

        if (evt != SCP_EVT_FAULT) continue;

        /* Hardware has already removed the gate. Record the firmware-side state. */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.scp_state = SCP_STATE_FAULT_DETECTED;
        g_sys.fault_active = true;
        g_sys.retry_count++;
        uint32_t retry_count = g_sys.retry_count;
        uint32_t pack_v = g_sys.pack_voltage_mv;
        op_mode_t restore_mode = g_sys.op_mode;
        uint32_t restore_duty = g_sys.pwm_duty_pct;
        xSemaphoreGive(g_state_mutex);

        protection_set_fault(PROT_FAULT_SCP, false);

        vTaskDelay(pdMS_TO_TICKS(10));
        int32_t trip_current = ina240_read_current_ma();
        black_box_write_fault(FAULT_SCP_TRIP, trip_current, pack_v);
        mqtt_publish_fault("SCP_TRIP", trip_current);
        /* Drop duplicate edges generated by the same already-latched hardware fault.
         * A genuine re-trip after a restored gate is queued again by the monitor below. */
        xQueueReset(scp_evt_queue);

        if (trip_window_reached()) {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state = SCP_STATE_PERMANENT_FAULT;
            g_sys.permanent_fault = true;
            g_sys.fault_active = true;
            xSemaphoreGive(g_state_mutex);
            protection_set_fault(PROT_FAULT_SCP, true);
            gate_disable();
            black_box_write_fault(FAULT_SCP_PERMANENT, trip_current, pack_v);
            mqtt_publish_fault("PERMANENT_FAULT", trip_current);

            /* Wait here until the MQTT reset command has proven the system safe. */
            for (;;) {
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                bool permanent = g_sys.permanent_fault;
                xSemaphoreGive(g_state_mutex);
                if (!permanent) break;
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            continue;
        }

        bool recovered = false;
        for (;;) {
            wait_recovery_interval();

            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state = SCP_STATE_RETRY_PROBE;
            restore_mode = g_sys.op_mode;
            restore_duty = g_sys.pwm_duty_pct;
            xSemaphoreGive(g_state_mutex);

            /* Probe only if an output had previously been requested. */
            if (restore_mode == MODE_FULL_POWER || restore_mode == MODE_PWM_50) {
                gate_enable_pwm(5U);
                vTaskDelay(pdMS_TO_TICKS(100));
                int32_t probe_current = ina240_read_current_ma();
                uint32_t abs_probe = (probe_current < 0) ? (uint32_t)(-probe_current) : (uint32_t)probe_current;
                uint32_t oc_limit = g_sys.oc_ma;
                uint32_t probe_threshold = (oc_limit > 0U) ? (oc_limit * SCP_PROBE_SHORTED_PCT) / 100U : 2000U;
                if (probe_threshold < 1000U) probe_threshold = 1000U;

                black_box_write_recovery_attempt(retry_count, abs_probe);

                if (abs_probe > probe_threshold || gpio_get_level(PIN_FAULT_N) == 0) {
                    gate_disable();
                    xQueueReset(scp_evt_queue);
                    mqtt_publish_fault("PROBE_SHORTED", probe_current);
                    ESP_LOGW(TAG, "Recovery probe failed: %d mA", probe_current);
                    continue;
                }
            } else {
                gate_disable();
            }

            /* Do not restore output if another protection source remains active. */
            if (!ltc6811_protection_conditions_clear()) {
                gate_disable();
                protection_clear_fault(PROT_FAULT_SCP);
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                g_sys.scp_state = SCP_STATE_NORMAL;
                g_sys.retry_count = 0U;
                xSemaphoreGive(g_state_mutex);
                mqtt_publish_fault("SCP_RECOVERY_BLOCKED", 0);
                recovered = true;
                break;
            }

            if (restore_mode == MODE_FULL_POWER) {
                gate_enable_full();
            } else if (restore_mode == MODE_PWM_50) {
                gate_enable_pwm(restore_duty);
            } else {
                gate_disable();
            }

            bool retrip = false;
            for (uint32_t ms = 0; ms < SCP_MONITOR_AFTER_RESTORE_MS; ms += 100U) {
                uint8_t pending = 0U;
                if (xQueuePeek(scp_evt_queue, &pending, pdMS_TO_TICKS(100)) == pdTRUE) {
                    retrip = true;
                    break;
                }
                if (gpio_get_level(PIN_FAULT_N) == 0) {
                    uint8_t evt = SCP_EVT_FAULT;
                    xQueueSend(scp_evt_queue, &evt, 0);
                    retrip = true;
                    break;
                }
            }
            if (retrip) {
                gate_disable();
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                g_sys.scp_state = SCP_STATE_FAULT_LATCH;
                xSemaphoreGive(g_state_mutex);
                recovered = true;
                break;
            }

            protection_clear_fault(PROT_FAULT_SCP);
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state = SCP_STATE_NORMAL;
            g_sys.fault_active = (g_sys.protection_fault_mask != 0U) || g_sys.permanent_fault;
            g_sys.retry_count = 0U;
            xSemaphoreGive(g_state_mutex);
            mqtt_publish_fault("RECOVERED", 0);
            recovered = true;
            break;
        }

        (void)recovered;
    }
}

void scp_recovery_enable_gate(void)
{
    if (!ltc6811_protection_conditions_clear()) {
        gate_disable();
        return;
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    op_mode_t mode = g_sys.op_mode;
    uint32_t duty = g_sys.pwm_duty_pct;
    xSemaphoreGive(g_state_mutex);

    if (mode == MODE_FULL_POWER) gate_enable_full();
    else if (mode == MODE_PWM_50) gate_enable_pwm(duty);
    else gate_disable();
}

void scp_clear_permanent_fault(void)
{
    if (!ltc6811_protection_conditions_clear()) {
        ESP_LOGW(TAG, "Reset rejected: active protection condition remains");
        mqtt_publish_fault("RESET_REJECTED_ACTIVE_FAULT", 0);
        return;
    }

    ltc6811_reset_fault_latches();
    protection_clear_fault(PROT_FAULT_SCP |
                           PROT_FAULT_TEMP_SENSOR |
                           PROT_FAULT_TEMP_SHUTDOWN |
                           PROT_FAULT_CELL_OV |
                           PROT_FAULT_CELL_UV |
                           PROT_FAULT_OVERCURRENT |
                           PROT_FAULT_PACK_OV);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.permanent_fault = false;
    g_sys.fault_active = false;
    g_sys.scp_state = SCP_STATE_NORMAL;
    g_sys.retry_count = 0U;
    trip_count_total = 0U;
    xSemaphoreGive(g_state_mutex);

    gate_disable();
    ESP_LOGI(TAG, "Permanent/protection faults cleared after safety recheck");
    mqtt_publish_fault("FAULT_RESET", 0);
}
