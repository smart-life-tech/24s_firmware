/*
 * Shared NVS/config support and centralized protection-state helpers.
 * The final PCB has no RF/PREBIAS legacy architecture.
 */

#include "config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define NVS_NS "24shub"
static const char *TAG = "CONFIG";

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi, uint32_t fallback)
{
    if (v < lo || v > hi) return fallback;
    return v;
}

void config_load_from_nvs(void)
{
    g_sys.ov_mv = OV_MV_LIFEPO4;
    g_sys.uv_mv = UV_MV_LIFEPO4;
    g_sys.oc_ma = OC_MA_DEFAULT;
    g_sys.bal_delta_mv = BAL_DELTA_MV;
    g_sys.pwm_duty_pct = 50U;
    g_sys.chemistry = CHEM_LIFEPO4;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint32_t v = 0U;
    if (nvs_get_u32(h, "ov_mv", &v) == ESP_OK)
        g_sys.ov_mv = clamp_u32(v, 2000U, 5000U, OV_MV_LIFEPO4);
    if (nvs_get_u32(h, "uv_mv", &v) == ESP_OK)
        g_sys.uv_mv = clamp_u32(v, 1500U, 4000U, UV_MV_LIFEPO4);
    if (nvs_get_u32(h, "oc_ma", &v) == ESP_OK)
        g_sys.oc_ma = clamp_u32(v, 1000U, 100000U, OC_MA_DEFAULT);
    if (nvs_get_u32(h, "bal_mv", &v) == ESP_OK)
        g_sys.bal_delta_mv = clamp_u32(v, 5U, 500U, BAL_DELTA_MV);
    if (nvs_get_u32(h, "duty", &v) == ESP_OK)
        g_sys.pwm_duty_pct = (v > 100U) ? 50U : v;

    uint8_t chem = 0U;
    if (nvs_get_u8(h, "chem", &chem) == ESP_OK) {
        g_sys.chemistry = (chem == CHEM_SODIUM_ION) ? CHEM_SODIUM_ION : CHEM_LIFEPO4;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: OV=%u UV=%u OC=%u BAL=%u CHEM=%d",
             g_sys.ov_mv, g_sys.uv_mv, g_sys.oc_ma,
             g_sys.bal_delta_mv, g_sys.chemistry);
}

void config_save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open NVS for config save");
        return;
    }

    esp_err_t err = ESP_OK;
    err |= nvs_set_u32(h, "ov_mv", g_sys.ov_mv);
    err |= nvs_set_u32(h, "uv_mv", g_sys.uv_mv);
    err |= nvs_set_u32(h, "oc_ma", g_sys.oc_ma);
    err |= nvs_set_u32(h, "bal_mv", g_sys.bal_delta_mv);
    err |= nvs_set_u32(h, "duty", g_sys.pwm_duty_pct);
    err |= nvs_set_u8(h, "chem", (uint8_t)g_sys.chemistry);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config NVS save failed: %s", esp_err_to_name(err));
    }
}

void protection_set_fault(uint32_t mask, bool latch_permanent)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.protection_fault_mask |= mask;
    g_sys.fault_active = true;
    if (latch_permanent) {
        g_sys.permanent_fault = true;
    }
    xSemaphoreGive(g_state_mutex);
}

void protection_clear_fault(uint32_t mask)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_sys.protection_fault_mask &= ~mask;
    g_sys.fault_active = (g_sys.protection_fault_mask != 0U) || g_sys.permanent_fault;
    xSemaphoreGive(g_state_mutex);
}

bool protection_has_fault(uint32_t mask)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    bool active = (g_sys.protection_fault_mask & mask) != 0U;
    xSemaphoreGive(g_state_mutex);
    return active;
}
