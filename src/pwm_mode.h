#pragma once
#include <stdint.h>

void pwm_set_duty(uint32_t duty_pct);
void pwm_start(uint32_t duty_pct);
void pwm_stop(void);
uint32_t pot_read_duty_pct(void);
void pwm_apply_pot(void);
