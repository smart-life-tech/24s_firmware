#pragma once
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void third_wire_task(void *arg);
void third_wire_set_mode_from_mqtt(const char *action);
void third_wire_set_pwm_duty(uint32_t duty_pct);
