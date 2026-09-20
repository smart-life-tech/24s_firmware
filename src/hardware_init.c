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

static void init_gpio(void)
{
    gpio_config_t gate_cfg = {
        .pin_bit_mask = (1ULL << PIN_GATE_CTRL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gate_cfg);
    gpio_set_level(PIN_GATE_CTRL, 0);

    gpio_config_t fault_cfg = {
        .pin_bit_mask = (1ULL << PIN_FAULT_N),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&fault_cfg);

    if (PIN_STATUS_LED != GPIO_NUM_NC) {
        gpio_config_t led_cfg = {
            .pin_bit_mask = (1ULL << PIN_STATUS_LED),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&led_cfg);
        gpio_set_level(PIN_STATUS_LED, 0);
    }

    gpio_install_isr_service(0);
}

static void init_ledc(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_SPEED_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_GATE_CTRL,
        .speed_mode = LEDC_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    ESP_LOGI(TAG, "LEDC 20kHz configured on GPIO %d", PIN_GATE_CTRL);
}

static void init_spi(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_SPI_MOSI,
        .miso_io_num   = PIN_SPI_MISO,
        .sclk_io_num   = PIN_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t ltc_dev = {
        .clock_speed_hz = 1000000,
        .mode           = 3,
        .spics_io_num   = PIN_LTC_CS,
        .queue_size     = 4,
    };
    spi_bus_add_device(SPI2_HOST, &ltc_dev, &g_spi_ltc);

    ESP_LOGI(TAG, "SPI bus initialized with LTC6811-1 on GPIO39/38");
}

static void init_adc(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(PIN_NTC_1, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(PIN_NTC_2, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(PIN_NTC_3, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(PIN_NTC_4, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(PIN_INA240_ADC, ADC_ATTEN_DB_11);

    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11,
                             ADC_WIDTH_BIT_12, 1100, &g_adc_chars);
    ESP_LOGI(TAG, "ADC initialized");
}

esp_err_t hardware_init(void)
{
    init_gpio();
    init_ledc();
    init_spi();
    init_adc();
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
    uint32_t raw = 0;
    for (int i = 0; i < 10; i++) raw += adc1_get_raw(channel);
    raw /= 10;
    return esp_adc_cal_raw_to_voltage(raw, &g_adc_chars);
}
