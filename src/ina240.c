#include "ina240.h"
#include "config.h"
#include "hardware_init.h"
#include "esp_log.h"

static const char *TAG = "INA240";

#define SHUNT_OHMS        0.0005f
#define INA240_GAIN_V_PER_V 20.0f
#define INA240_VREF_MV    1650

int32_t ina240_read_current_ma(void)
{
    uint32_t v_mv = adc_read_mv(PIN_INA240_ADC);
    int32_t v_diff_mv = (int32_t)v_mv - INA240_VREF_MV;
    /* INA240 transfer: Vdiff = I * Rshunt * Gain, with 0.5mΩ shunt and 20V/V gain.
     * For a 0.5mΩ shunt, 1A produces 10mV at the INA output (relative to VREF).
     * So current in mA is Vdiff_mV / (Rshunt * Gain), which = Vdiff_mV / 0.01.
     */
    float current_ma = (float)v_diff_mv / (SHUNT_OHMS * INA240_GAIN_V_PER_V);
    return (int32_t)current_ma;
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
