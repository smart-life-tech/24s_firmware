#pragma once
#include "esp_err.h"
#include <stdint.h>

int32_t ina240_read_current_ma(void);
esp_err_t ina240_verify_idle(void);
