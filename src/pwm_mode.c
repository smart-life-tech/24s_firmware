/**
 * pwm_mode.c ” LEDC PWM helpers
 *
 * The INA240A1D has built-in enhanced PWM rejection rated to 100kHz.
 * Our 20kHz target is well within spec ” no external blanking needed.
 * TLV3501 OCP threshold remains valid during PWM switching.
 */


#include "pwm_mode.h"
#include "config.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/adc.h"         
#include "esp_adc_cal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "PWM";

extern esp_adc_cal_characteristics_t adc_chars;

/**
 * Set duty cycle 0-100%.
 * Uses 13-bit LEDC timer: max counts = 8191.
 * 50% = 4096 counts (default PWM mode).
 */
void pwm_set_duty(uint32_t duty_pct)
{
    if (duty_pct > 100) duty_pct = 100;

    /* 13-bit max = 8191 */
    uint32_t duty_counts = (duty_pct * 8191UL) / 100UL;

    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty_counts);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);

    ESP_LOGD(TAG, "PWM duty: %d%% (%d counts)", duty_pct, duty_counts);
}

void pwm_start(uint32_t duty_pct)
{
    pwm_set_duty(duty_pct);
    ESP_LOGI(TAG, "PWM started @ %d%% 20kHz", duty_pct);
}

void pwm_stop(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    ESP_LOGI(TAG, "PWM stopped");
}


uint32_t pot_read_duty_pct(void)
{
    
    uint32_t accumulator = 0U;

    for (int i = 0; i < POT_OVERSAMPLE_N; i++) {
        int raw = adc1_get_raw(PIN_POT_ADC_CH);
        if (raw < 0) {
            
            ESP_LOGW(TAG, "pot_read_duty_pct: adc1_get_raw error sample %d", i);
            raw = 0;
        }
        accumulator += (uint32_t)raw;
    }

    uint32_t adc_avg = accumulator >> 5;

    uint32_t duty_pct;

    if (adc_avg <= POT_DEADBAND_LOW) {
        duty_pct = 0U;

    } else if (adc_avg >= POT_DEADBAND_HIGH) {
        duty_pct = 100U;

    } else {
       
        uint32_t span      = POT_DEADBAND_HIGH - POT_DEADBAND_LOW; 
        uint32_t numerator = (adc_avg - POT_DEADBAND_LOW) * 100UL;
        duty_pct           = numerator / span;

        if (duty_pct > 100U) duty_pct = 100U;
        if (duty_pct < 1U)   duty_pct = 1U;
    }

    if (gpio_get_level(MASTER_SWITCH_GPIO) == 1) {
        ESP_LOGD(TAG, "pot: Master Switch HIGH : duty forced 100%%");
        return 100U;
    }

    ESP_LOGD(TAG, "pot: adc_avg=%u  duty=%u%%", adc_avg, duty_pct);
    return duty_pct;
}


void pwm_apply_pot(void)
{
    uint32_t duty = pot_read_duty_pct();

    pwm_set_duty(duty);

    
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_sys.pwm_duty_pct = duty;
        xSemaphoreGive(g_state_mutex);
    } else {
        ESP_LOGW(TAG, "pwm_apply_pot: state mutex timeout ” "
                      "duty=%u%% applied, g_sys.pwm_duty_pct not updated this cycle",
                 (unsigned)duty);
    }
}
