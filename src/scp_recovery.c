/**
 * scp_recovery.c — Hardware Short-Circuit Protection + 10-Second Auto-Recovery
 *
 * Architecture:
 *  - TLV3501 → ISO5451DW: hardware trip in <500ns (no firmware in loop)
 *  - FAULT_N GPIO falling-edge ISR fires AFTER hardware has already cut the gate
 *  - This task manages the recovery state machine described in Section 6
 *
 * State machine:  NORMAL → FAULT_DETECTED → FAULT_LATCH
 *                 → RECOVERY_WAIT (10s) → RETRY_PROBE
 *                 → GATE_RESTORE → NORMAL
 *                 (3 trips in 60s) → PERMANENT_FAULT
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

static const char *TAG = "SCP";

/* Queue from ISR → task */
static QueueHandle_t scp_evt_queue;

/* Trip timestamps for 3-in-60s window */
static int64_t trip_times_us[SCP_MAX_RETRIES];
static int     trip_idx = 0;

/* ----------------------------------------------------------------
 *  FAULT_N GPIO ISR — executes in interrupt context, very fast
 * ---------------------------------------------------------------- */
static void IRAM_ATTR fault_n_isr_handler(void *arg)
{
    uint8_t evt = 1;
    xQueueSendFromISR(scp_evt_queue, &evt, NULL);
    /* NOTE: gate is already OFF — TLV3501 cut ISO5451DW before we got here */
}

/* ----------------------------------------------------------------
 *  Gate control helpers
 * ---------------------------------------------------------------- */
static void gate_enable_full(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 1);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_FULL_POWER;
    xSemaphoreGive(g_state_mutex);
    ESP_LOGI(TAG, "Gate → FULL POWER");
}

static void gate_enable_pwm(uint32_t duty_pct)
{
    uint32_t duty = (duty_pct * LEDC_DUTY_MAX) / 100UL;
    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_PWM;
    xSemaphoreGive(g_state_mutex);
    ESP_LOGI(TAG, "Gate → PWM %d%%", duty_pct);
}

static void gate_disable(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 0);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_OFF;
    xSemaphoreGive(g_state_mutex);
    ESP_LOGI(TAG, "Gate → OFF");
}

/* ----------------------------------------------------------------
 *  Check if 3 trips happened within 60-second window
 * ---------------------------------------------------------------- */
static bool check_permanent_fault(void)
{
    int64_t now = esp_timer_get_time();
    /* Store this trip time */
    trip_times_us[trip_idx % SCP_MAX_RETRIES] = now;
    trip_idx++;

    if (trip_idx < SCP_MAX_RETRIES) return false;

    /* Check if oldest of last 3 is within 60s */
    int64_t oldest = trip_times_us[trip_idx % SCP_MAX_RETRIES];
    if ((now - oldest) <= ((int64_t)SCP_RETRY_WINDOW_MS * 1000LL)) {
        return true;  // 3 trips in 60 seconds
    }
    return false;
}

/* ----------------------------------------------------------------
 *  LED helpers
 * ---------------------------------------------------------------- */
static void led_blink_task_red(void *arg)
{
    if (PIN_STATUS_LED == GPIO_NUM_NC) {
        vTaskDelete(NULL);
        return;
    }

    /* 5Hz red blink during recovery wait */
    while (1) {
        gpio_set_level(PIN_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ----------------------------------------------------------------
 *  Main SCP state machine task
 * ---------------------------------------------------------------- */
void scp_recovery_task(void *arg)
{
    scp_evt_queue = xQueueCreate(4, sizeof(uint8_t));

    /* Register ISR for FAULT_N falling edge */
    gpio_isr_handler_add(PIN_FAULT_N, fault_n_isr_handler, NULL);

    if (gpio_get_level(PIN_FAULT_N) == 0) {
        uint8_t evt = 1;
        xQueueSend(scp_evt_queue, &evt, 0);
        ESP_LOGW(TAG, "FAULT_N already low at boot; recovery task queued immediate retry");
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.scp_state = SCP_STATE_NORMAL;
    xSemaphoreGive(g_state_mutex);

    ESP_LOGI(TAG, "SCP recovery task running");

    uint8_t evt;
    TaskHandle_t led_task_handle = NULL;

    for (;;) {
        /* Block until ISR signals a fault (or timeout for periodic checks) */
        if (xQueueReceive(scp_evt_queue, &evt, pdMS_TO_TICKS(500)) != pdTRUE) {
            /* Periodic: update INA240 current reading */
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.pack_current_ma = ina240_read_current_ma();
            xSemaphoreGive(g_state_mutex);
            continue;
        }

        /* ---- FAULT DETECTED ---- */
        ESP_LOGW(TAG, "FAULT_N triggered — hardware has cut gate");
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.scp_state   = SCP_STATE_FAULT_DETECTED;
        g_sys.fault_active = true;
        xSemaphoreGive(g_state_mutex);

        /* ---- FAULT LATCH: capture peak current, log to Black Box ---- */
        vTaskDelay(pdMS_TO_TICKS(10));  // allow INA240 to settle
        int32_t peak_current = ina240_read_current_ma();
        uint32_t pack_v      = g_sys.pack_voltage_mv;

        ESP_LOGW(TAG, "Peak fault current: %d mA", peak_current);

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.scp_state    = SCP_STATE_FAULT_LATCH;
        g_sys.retry_count++;
        xSemaphoreGive(g_state_mutex);

        black_box_write_fault(FAULT_SCP_TRIP, peak_current, pack_v);
        mqtt_publish_fault("SCP_TRIP", peak_current);

        /* Check for permanent fault (3 in 60s) */
        if (check_permanent_fault()) {
            ESP_LOGE(TAG, "PERMANENT FAULT — 3 trips in 60s. Reset required.");
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state    = SCP_STATE_PERMANENT_FAULT;
            g_sys.permanent_fault = true;
            xSemaphoreGive(g_state_mutex);
            black_box_write_fault(FAULT_SCP_PERMANENT, peak_current, pack_v);
            mqtt_publish_fault("PERMANENT_FAULT", peak_current);
            /* Blink LED — wait for reset or MQTT clear */
            while (g_sys.permanent_fault) {
                if (PIN_STATUS_LED != GPIO_NUM_NC) {
                    gpio_set_level(PIN_STATUS_LED, 1);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    gpio_set_level(PIN_STATUS_LED, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            /* MQTT reset_fault command cleared permanent_fault */
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state    = SCP_STATE_NORMAL;
            g_sys.fault_active = false;
            g_sys.retry_count  = 0;
            trip_idx = 0;
            xSemaphoreGive(g_state_mutex);
            ESP_LOGI(TAG, "Permanent fault cleared — resuming");
            continue;
        }

        /* ---- RECOVERY WAIT: 10 seconds, gate off, LED blinks red 5Hz ---- */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.scp_state = SCP_STATE_RECOVERY_WAIT;
        xSemaphoreGive(g_state_mutex);

        xTaskCreate(led_blink_task_red, "led_red", 1024, NULL, 1, &led_task_handle);
        mqtt_publish_recovery_eta(SCP_RECOVERY_WAIT_MS / 1000);
        ESP_LOGI(TAG, "Recovery wait: 10 seconds...");
        vTaskDelay(pdMS_TO_TICKS(SCP_RECOVERY_WAIT_MS));

        /* Stop LED blink task */
        if (led_task_handle) {
            vTaskDelete(led_task_handle);
            led_task_handle = NULL;
            if (PIN_STATUS_LED != GPIO_NUM_NC) {
                gpio_set_level(PIN_STATUS_LED, 0);
            }
        }

        /* ---- RETRY PROBE: apply a limited 5% drive only during the probe ---- */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_sys.scp_state = SCP_STATE_RETRY_PROBE;
        op_mode_t restore_mode = g_sys.op_mode;
        uint32_t  restore_duty = g_sys.pwm_duty_pct;
        xSemaphoreGive(g_state_mutex);

        uint32_t probe_duty_pct = 5U;
        if (restore_mode == MODE_PWM_50 && restore_duty > 5U) {
            probe_duty_pct = 5U;
        } else if (restore_mode == MODE_PWM_50 && restore_duty <= 5U) {
            probe_duty_pct = restore_duty;
        }

        if (restore_mode == MODE_FULL_POWER || restore_mode == MODE_PWM_50) {
            gate_enable_pwm(probe_duty_pct);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        int32_t probe_current = ina240_read_current_ma();
        uint32_t abs_probe_current = (probe_current < 0) ? (uint32_t)(-probe_current) : (uint32_t)probe_current;
        uint32_t probe_threshold_ma = (g_sys.oc_ma > 0U) ? ((g_sys.oc_ma * SCP_PROBE_SHORTED_PCT) / 100U) : 2000U;

        ESP_LOGI(TAG, "Retry probe: current %d mA, threshold %u mA, duty %u%%", probe_current, probe_threshold_ma, probe_duty_pct);

        black_box_write_recovery_attempt(g_sys.retry_count, abs_probe_current);

        if (abs_probe_current > probe_threshold_ma) {
            /* Load still shorted — restore the safe OFF state and retry later. */
            ESP_LOGW(TAG, "Load still shorted: current %d mA > %u mA", probe_current, probe_threshold_ma);
            gate_disable();
            mqtt_publish_fault("PROBE_SHORTED", probe_current);
            xQueueSend(scp_evt_queue, &evt, 0);  // re-queue to keep recovery state machine alive
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state = SCP_STATE_RECOVERY_WAIT;
            xSemaphoreGive(g_state_mutex);
            vTaskDelay(pdMS_TO_TICKS(SCP_RECOVERY_WAIT_MS));
            continue;
        }

        /* MODE_OFF: gate stays off intentionally */

        /* Monitor closely for 2 seconds. This is an explicit re-trip check rather
         * than relying on a fresh FAULT_N falling edge after the signal is already low. */
        bool re_trip = false;
        for (int i = 0; i < 20; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(PIN_FAULT_N) == 0) {
                re_trip = true;
                break;
            }
        }

        if (re_trip) {
            ESP_LOGW(TAG, "Re-trip within 2s monitor window — forcing gate off");
            gate_disable();
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state = SCP_STATE_FAULT_DETECTED;
            g_sys.fault_active = true;
            xSemaphoreGive(g_state_mutex);
            xQueueSend(scp_evt_queue, &evt, 0);
            continue;
        } else {
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_sys.scp_state    = SCP_STATE_NORMAL;
            g_sys.fault_active = false;
            xSemaphoreGive(g_state_mutex);

            if (restore_mode == MODE_FULL_POWER) {
                gate_enable_full();
            } else if (restore_mode == MODE_PWM_50) {
                gate_enable_pwm(restore_duty);
            } else {
                gate_disable();
            }

            if (PIN_STATUS_LED != GPIO_NUM_NC) {
                gpio_set_level(PIN_STATUS_LED, 1);  // solid on = OK
            }
            ESP_LOGI(TAG, "Gate restored — NORMAL (mode=%d duty=%u%%)", restore_mode, restore_duty);
            mqtt_publish_fault("RECOVERED", 0);
        }
    }
}

/* ----------------------------------------------------------------
 *  Apply the current persisted gate mode after an SCP recovery state change.
 * ---------------------------------------------------------------- */
void scp_recovery_enable_gate(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    op_mode_t mode = g_sys.op_mode;
    uint32_t  duty = g_sys.pwm_duty_pct;
    xSemaphoreGive(g_state_mutex);

    if (mode == MODE_FULL_POWER) {
        gate_enable_full();
    } else if (mode == MODE_PWM_50) {
        gate_enable_pwm(duty);
    }
    /* Default mode is OFF — the gate remains disabled until software chooses a valid mode. */
}

/* ----------------------------------------------------------------
 *  Called by MQTT reset_fault command handler
 * ---------------------------------------------------------------- */
void scp_clear_permanent_fault(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.permanent_fault = false;
    g_sys.fault_active     = false;
    g_sys.scp_state       = SCP_STATE_NORMAL;
    g_sys.gate_state      = GATE_OFF;
    g_sys.fault_count     = 0;
    xSemaphoreGive(g_state_mutex);
    ltc6811_reset_fault_latches();
    gate_disable();
    ESP_LOGI(TAG, "Permanent fault cleared by MQTT command");
}
