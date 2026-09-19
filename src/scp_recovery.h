#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void scp_recovery_task(void *arg);
void scp_recovery_enable_gate(void);
void scp_clear_permanent_fault(void);
