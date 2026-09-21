/**
 * hardware_init.c — GPIO, LEDC PWM, SPI, ADC peripheral setup
 */

#include "hardware_init.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "HW_INIT";

esp_adc_cal_characteristics_t g_adc_chars;
spi_device_handle_t g_spi_ltc;

static esp_err_t init_gpio(void)
{
    gpio_config_t gate_cfg = {
        .pin_bit_mask = (1ULL << PIN_GATE_CTRL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gate_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_GATE_CTRL, 0);

    gpio_config_t fault_cfg = {
        .pin_bit_mask = (1ULL << PIN_FAULT_N),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&fault_cfg);
    if (err != ESP_OK) return err;

    if (PIN_STATUS_LED != GPIO_NUM_NC) {
        gpio_config_t led_cfg = {
            .pin_bit_mask = (1ULL << PIN_STATUS_LED),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&led_cfg);
        if (err != ESP_OK) return err;
        gpio_set_level(PIN_STATUS_LED, 0);
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t init_ledc(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_SPEED_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_GATE_CTRL,
        .speed_mode = LEDC_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "LEDC 20kHz configured on GPIO %d", PIN_GATE_CTRL);
    return ESP_OK;
}

static esp_err_t init_spi(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_SPI_MOSI,
        .miso_io_num   = PIN_SPI_MISO,
        .sclk_io_num   = PIN_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t ltc_dev = {
        .clock_speed_hz = 1000000,
        .mode           = 3,
        .spics_io_num   = PIN_LTC_CS,
        .queue_size     = 4,
    };
    err = spi_bus_add_device(SPI2_HOST, &ltc_dev, &g_spi_ltc);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "SPI bus initialized with LTC6811-1 on GPIO39/38");
    return ESP_OK;
}

static esp_err_t init_adc(void)
{
    esp_err_t err = adc1_config_width(ADC_WIDTH_BIT_12);
    if (err != ESP_OK) return err;
    err = adc1_config_channel_atten(PIN_NTC_1, ADC_ATTEN_DB_11);
    if (err != ESP_OK) return err;
    err = adc1_config_channel_atten(PIN_NTC_2, ADC_ATTEN_DB_11);
    if (err != ESP_OK) return err;
    err = adc1_config_channel_atten(PIN_NTC_3, ADC_ATTEN_DB_11);
    if (err != ESP_OK) return err;
    err = adc1_config_channel_atten(PIN_NTC_4, ADC_ATTEN_DB_11);
    if (err != ESP_OK) return err;
    err = adc1_config_channel_atten(PIN_INA240_ADC, ADC_ATTEN_DB_11);
    if (err != ESP_OK) return err;

    /* esp_adc_cal_characterize() returns a calibration source enum, not ESP_OK.
     * The call is valid for ESP32-S2 ADC calibration and should not be treated as an error code. */
    (void)esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11,
                                    ADC_WIDTH_BIT_12, 1100, &g_adc_chars);
    ESP_LOGI(TAG, "ADC initialized");
    return ESP_OK;
}

esp_err_t hardware_init(void)
{
    esp_err_t err = init_gpio();
    if (err != ESP_OK) return err;
    err = init_ledc();
    if (err != ESP_OK) return err;
    err = init_spi();
    if (err != ESP_OK) return err;
    err = init_adc();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Hardware init complete");
    return ESP_OK;
}

void led_blink_fault(void)
{
    if (PIN_STATUS_LED == GPIO_NUM_NC) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        gpio_set_level(PIN_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_blink_amber_1hz(void)
{
    if (PIN_STATUS_LED == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(PIN_STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_STATUS_LED, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
}

void gate_hold_off(void)
{
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 0);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.gate_state = GATE_OFF;
    xSemaphoreGive(g_state_mutex);
}

uint32_t adc_read_mv(adc1_channel_t channel)
{
    uint32_t raw_sum = 0U;
    uint32_t valid_samples = 0U;

    for (int i = 0; i < 10; i++) {
        int raw = adc1_get_raw(channel);
        if (raw >= 0) {
            raw_sum += (uint32_t)raw;
            valid_samples++;
        }
    }

    if (valid_samples == 0U) {
        return 0U;
    }

    uint32_t raw_avg = raw_sum / valid_samples;
    return esp_adc_cal_raw_to_voltage(raw_avg, &g_adc_chars);
}
