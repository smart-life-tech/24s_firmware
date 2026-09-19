/**
 * hardware_init.c  GPIO, LEDC PWM, SPI, ADC peripheral setup
 */

#include "hardware_init.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

static const char *TAG = "HW_INIT";

static esp_adc_cal_characteristics_t adc_chars;
spi_device_handle_t g_spi_ltc;
spi_device_handle_t g_spi_nrf;

/* ----------------------------------------------------------------
 *  GPIO
 * ---------------------------------------------------------------- */
static void init_gpio(void)
{
    /* Gate control output, default LOW (gate off) */
    gpio_config_t gate_cfg = {
        .pin_bit_mask = (1ULL << PIN_GATE_CTRL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gate_cfg);
    gpio_set_level(PIN_GATE_CTRL, 0);  // hard off

    /* Third Wire  input with interrupt on rising edge */
    gpio_config_t tw_cfg = {
        .pin_bit_mask = (1ULL << PIN_THIRD_WIRE),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&tw_cfg);

    /* FAULT_N  input with interrupt on falling edge (active low) */
    gpio_config_t fault_cfg = {
        .pin_bit_mask = (1ULL << PIN_FAULT_N),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&fault_cfg);

    /* Status LED  output */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << PIN_STATUS_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);

    /* nRF CE and CSN  outputs */
    gpio_config_t nrf_cfg = {
        .pin_bit_mask = (1ULL << PIN_NRF_CE) | (1ULL << PIN_NRF_CSN),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&nrf_cfg);
    gpio_set_level(PIN_NRF_CE, 0);
    gpio_set_level(PIN_NRF_CSN, 1);  // deselect

    /* Install ISR service (shared for both FAULT_N and THIRD_WIRE) */
    gpio_install_isr_service(0);
}

/* ----------------------------------------------------------------
 *  LEDC PWM (20kHz, 13-bit, on GATE_CTRL pin)
 * ---------------------------------------------------------------- */
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
        .duty       = 0,      // start at 0%  gate off
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    /* Do NOT call ledc_fade_func_install here if not using fade */
    ESP_LOGI(TAG, "LEDC 20kHz 13-bit configured on GPIO %d", PIN_GATE_CTRL);
}

/* ----------------------------------------------------------------
 *  SPI (FSPI  shared bus, separate CS for LTC6813 and nRF24L01P)
 * ---------------------------------------------------------------- */
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

    /* LTC6813 device: 1MHz, SPI mode 3 (CPOL=1, CPHA=1) */
    spi_device_interface_config_t ltc_dev = {
        .clock_speed_hz = 1000000,
        .mode           = 3,
        .spics_io_num   = PIN_LTC_CS,
        .queue_size     = 4,
    };
    spi_bus_add_device(SPI2_HOST, &ltc_dev, &g_spi_ltc);

    /* nRF24L01P device: 8MHz, SPI mode 0 */
    spi_device_interface_config_t nrf_dev = {
        .clock_speed_hz = 8000000,
        .mode           = 0,
        .spics_io_num   = PIN_NRF_CSN,
        .queue_size     = 4,
    };
    spi_bus_add_device(SPI2_HOST, &nrf_dev, &g_spi_nrf);

    ESP_LOGI(TAG, "SPI bus initialized");
}

/* ----------------------------------------------------------------
 *  ADC (12-bit, 11dB attenuation  0-3.9V range)
 * ---------------------------------------------------------------- */
static void init_adc(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(PIN_PREBIAS_ADC, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(PIN_INA240_ADC,  ADC_ATTEN_DB_11);

    adc1_config_channel_atten(PIN_POT_ADC_CH, ADC_ATTEN_DB_11);

    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11,
                             ADC_WIDTH_BIT_12, 1100, &adc_chars);
    ESP_LOGI(TAG, "ADC initialized");
}

/* ----------------------------------------------------------------
 *  Public entry point
 * ---------------------------------------------------------------- */
esp_err_t hardware_init(void)
{
    init_gpio();
    init_ledc();
    init_spi();
    init_adc();
    ESP_LOGI(TAG, "Hardware init complete");
    return ESP_OK;
}

void gate_hold_off(void)
{
    /* Stop LEDC and drive GPIO low  belt + suspenders */
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    gpio_set_level(PIN_GATE_CTRL, 0);
}

uint32_t adc_read_mv(adc1_channel_t channel)
{
    uint32_t raw = 0;
    for (int i = 0; i < 10; i++) raw += adc1_get_raw(channel);
    raw /= 10;
    return esp_adc_cal_raw_to_voltage(raw, &adc_chars);
}
