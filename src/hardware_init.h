#pragma once
#include "esp_err.h"
#include "driver/adc.h"

esp_err_t hardware_init(void);
void gate_hold_off(void);
uint32_t adc_read_mv(adc1_channel_t channel);
void led_blink_fault(void);
void led_blink_amber_1hz(void);
