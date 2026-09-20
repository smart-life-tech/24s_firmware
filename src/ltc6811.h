#pragma once
#include "esp_err.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t ltc6811_self_test(void);
esp_err_t ltc6811_read_all_cells(uint16_t *cell_mv_out);
void ltc6811_reset_fault_latches(void);
void cell_monitor_task(void *arg);
