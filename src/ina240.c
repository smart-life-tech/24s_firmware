#include "ina240.h"
#include "config.h"
#include "hardware_init.h"
#include "esp_log.h"

static const char *TAG = "INA240";

#define RSENSE_MOHM 500
#define INA240_GAIN 20
#define VREF_MV    1650

int32_t ina240_read_current_ma(void)
{
    uint32_t v_mv = adc_read_mv(PIN_INA240_ADC);
    int32_t v_diff_mv = (int32_t)v_mv - VREF_MV;
    int32_t current_ma = (v_diff_mv * 1000) / ((int32_t)RSENSE_MOHM * INA240_GAIN / 1000);
    return current_ma;
}

esp_err_t ina240_verify_idle(void)
{
    int32_t current_ma = ina240_read_current_ma();
    if (current_ma > 2000 || current_ma < -2000) {
        ESP_LOGE(TAG, "Idle check fail: %ld mA", (long)current_ma);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Idle check OK: %ld mA", (long)current_ma);
    return ESP_OK;
}
