/*
 * Shared NVS/config support retained in a dedicated module to keep the
 * final PCB firmware free from the removed legacy RF/PREBIAS architecture.
 */

#include "config.h"
#include "nvs.h"

#define NVS_NS "24shub"

void config_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        g_sys.ov_mv = OV_MV_LIFEPO4;
        g_sys.uv_mv = UV_MV_LIFEPO4;
        g_sys.oc_ma = OC_MA_DEFAULT;
        g_sys.bal_delta_mv = BAL_DELTA_MV;
        g_sys.pwm_duty_pct = 50;
        g_sys.chemistry = CHEM_LIFEPO4;
        return;
    }

    uint32_t v = 0;
    if (nvs_get_u32(h, "ov_mv", &v) == ESP_OK) g_sys.ov_mv = v;
    else g_sys.ov_mv = OV_MV_LIFEPO4;
    if (nvs_get_u32(h, "uv_mv", &v) == ESP_OK) g_sys.uv_mv = v;
    else g_sys.uv_mv = UV_MV_LIFEPO4;
    if (nvs_get_u32(h, "oc_ma", &v) == ESP_OK) g_sys.oc_ma = v;
    else g_sys.oc_ma = OC_MA_DEFAULT;
    if (nvs_get_u32(h, "bal_mv", &v) == ESP_OK) g_sys.bal_delta_mv = v;
    else g_sys.bal_delta_mv = BAL_DELTA_MV;
    if (nvs_get_u32(h, "duty", &v) == ESP_OK) g_sys.pwm_duty_pct = v;
    else g_sys.pwm_duty_pct = 50;
    if (nvs_get_u8(h, "chem", &v) == ESP_OK) g_sys.chemistry = (v == CHEM_SODIUM_ION) ? CHEM_SODIUM_ION : CHEM_LIFEPO4;
    else g_sys.chemistry = CHEM_LIFEPO4;

    nvs_close(h);
}

void config_save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "ov_mv", g_sys.ov_mv);
    nvs_set_u32(h, "uv_mv", g_sys.uv_mv);
    nvs_set_u32(h, "oc_ma", g_sys.oc_ma);
    nvs_set_u32(h, "bal_mv", g_sys.bal_delta_mv);
    nvs_set_u32(h, "duty", g_sys.pwm_duty_pct);
    nvs_set_u8(h, "chem", (uint8_t)g_sys.chemistry);
    nvs_commit(h);
    nvs_close(h);
}

