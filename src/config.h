/**
 * config.h — 24S Smart Hub central configuration
 * Hardware map matches the reconciled PCB: GPIO4 gate/PWM, GPIO39/38 SPI,
 * and four dedicated NTC inputs on GPIO3/7/9/10.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"

/* ================================================================
 *  GPIO pin map (ESP32-S2)
 * ================================================================ */

#define PIN_GATE_CTRL       GPIO_NUM_4
#define PIN_FAULT_N         GPIO_NUM_6
#define PIN_STATUS_LED      GPIO_NUM_NC /* GPIO7 is dedicated to NTC2 on the final PCB; no software LED is placed on this pin */
#define PIN_LTC_CS          GPIO_NUM_13

#define PIN_SPI_MOSI        GPIO_NUM_39
#define PIN_SPI_MISO        GPIO_NUM_37
#define PIN_SPI_CLK         GPIO_NUM_38

#define PIN_POT_ADC_CH      ADC1_CHANNEL_7   // GPIO8
#define PIN_POT_ADC_GPIO    GPIO_NUM_8
#define MASTER_SWITCH_GPIO  GPIO_NUM_NC

#define POT_OVERSAMPLE_N    32
#define POT_DEADBAND_LOW    82
#define POT_DEADBAND_HIGH   4013
#define POT_POLL_INTERVAL_MS 50

/* INA240 current-sense ADC */
#define PIN_INA240_ADC      ADC1_CHANNEL_1   // GPIO2

/* 4 NTC inputs mapped as specified on the PCB */
#define PIN_NTC_1           ADC1_CHANNEL_2   // GPIO3
#define PIN_NTC_2           ADC1_CHANNEL_6   // GPIO7
#define PIN_NTC_3           ADC1_CHANNEL_8   // GPIO9
#define PIN_NTC_4           ADC1_CHANNEL_9   // GPIO10

/* LEDC PWM output on gate control pin */
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_SPEED_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_FREQ_HZ        20000
#define LEDC_RESOLUTION     LEDC_TIMER_13_BIT
#define LEDC_DUTY_50PCT     4096

/* ================================================================
 *  Cell thresholds
 * ================================================================ */
typedef enum {
    CHEM_LIFEPO4 = 0,
    CHEM_SODIUM_ION
} cell_chemistry_t;

#define OV_MV_LIFEPO4       3650
#define UV_MV_LIFEPO4       2500
#define OV_MV_NA_ION        4000
#define UV_MV_NA_ION        2800

#define BAL_DELTA_MV        30
#define BAL_STOP_MV         15
#define PACK_CUTOFF_MV      93000U
#define CELL_COUNT          24
#define NTC_COUNT           4

#define TEMP_WARN_C         60
#define TEMP_SHUTDOWN_C     80
#define OC_MA_DEFAULT       40000

/* ================================================================
 *  Timing constants
 * ================================================================ */
#define CELL_SCAN_INTERVAL_MS       250
#define TEMP_SCAN_INTERVAL_MS       500
#define TELEMETRY_INTERVAL_MS       5000
#define CELL_REPORT_INTERVAL_MS     30000
#define BLACKBOX_LOG_INTERVAL_MS    60000
#define INA240_POLL_INTERVAL_MS     100
#define SCP_RECOVERY_WAIT_MS        10000
#define SCP_MAX_RETRIES             3
#define SCP_RETRY_WINDOW_MS         60000
#define SCP_MONITOR_AFTER_RESTORE_MS 2000
#define SCP_PROBE_SHORTED_PCT       5
#define THIRD_WIRE_DEBOUNCE_MS      200

/* ================================================================
 *  Shared state types
 * ================================================================ */

typedef enum {
    GATE_OFF = 0,
    GATE_FULL_POWER,
    GATE_PWM
} gate_state_t;

typedef enum {
    MODE_OFF = 0,
    MODE_FULL_POWER,
    MODE_PWM_50
} op_mode_t;

typedef enum {
    SCP_STATE_NORMAL = 0,
    SCP_STATE_FAULT_DETECTED,
    SCP_STATE_FAULT_LATCH,
    SCP_STATE_RECOVERY_WAIT,
    SCP_STATE_RETRY_PROBE,
    SCP_STATE_GATE_RESTORE,
    SCP_STATE_PERMANENT_FAULT
} scp_state_t;

typedef enum {
    OUTPUT_PROFILE_STANDBY = 0,
    OUTPUT_PROFILE_18V,
    OUTPUT_PROFILE_36V,
    OUTPUT_PROFILE_48V,
    OUTPUT_PROFILE_72V
} output_profile_t;

typedef struct {
    gate_state_t      gate_state;
    op_mode_t         op_mode;
    scp_state_t       scp_state;
    output_profile_t  output_profile;
    uint8_t           fault_count;
    bool              permanent_fault;
    bool              wifi_connected;
    bool              boot_complete;
    uint32_t          pwm_duty_pct;
    int32_t           pack_current_ma;
    uint32_t          pack_voltage_mv;
    uint16_t          cell_mv[CELL_COUNT];
    bool              cell_balancing[CELL_COUNT];
    float             temp_avg_c;
    float             temp_sensors_c[NTC_COUNT];
    int8_t            wifi_rssi;
    uint32_t          soc_percent_x10;
    uint32_t          retry_count;
    bool              fault_active;
    uint32_t          ov_mv;
    uint32_t          uv_mv;
    uint32_t          oc_ma;
    uint32_t          bal_delta_mv;
    cell_chemistry_t  chemistry;
} system_state_t;

typedef enum {
    FAULT_NONE              = 0x0000,
    FAULT_CELL_MONITOR_FAIL = 0x0001,
    FAULT_INA240_FAIL       = 0x0002,
    FAULT_SCP_TRIP          = 0x0020,
    FAULT_SCP_RECOVERY      = 0x0021,
    FAULT_SCP_PERMANENT     = 0x0022,
    FAULT_CELL_OV           = 0x0030,
    FAULT_CELL_UV           = 0x0031,
    FAULT_PACK_OV           = 0x0032,
    FAULT_TEMP_WARN         = 0x0040,
    FAULT_TEMP_SHUTDOWN     = 0x0041,
} fault_type_t;

extern system_state_t g_sys;
extern SemaphoreHandle_t g_state_mutex;

void config_load_from_nvs(void);
void config_save_to_nvs(void);
