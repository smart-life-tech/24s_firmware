/**
 * config.h — 24S Smart Hub Central Configuration
 * All GPIO assignments, thresholds, timing constants, and shared types.
 * Change hardware mappings HERE only — never scatter magic numbers in source files.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 *  GPIO PIN MAP  (ESP32-S2-WROVER-I)
 * ================================================================ */

/* Gate control → ISO5451DW enable input */
#define PIN_GATE_CTRL       GPIO_NUM_4

/* Third Wire input (J9 / J11) */
#define PIN_THIRD_WIRE      GPIO_NUM_5

/* Hardware fault line from TLV3501 → ISO5451DW, also to ESP32 interrupt */
#define PIN_FAULT_N         GPIO_NUM_6    // active LOW

/* Status LED */
#define PIN_STATUS_LED      GPIO_NUM_7

/* nRF24L01P SPI (FSPI bus shared with LTC6813 via CS lines) */
#define PIN_NRF_CE          GPIO_NUM_10
#define PIN_NRF_CSN         GPIO_NUM_11
#define PIN_NRF_IRQ         GPIO_NUM_12

/* LTC6813 isoSPI chip-select */
#define PIN_LTC_CS          GPIO_NUM_13

/* Pre-bias ADC (load-side voltage divider → J7) */
#define PIN_PREBIAS_ADC     ADC1_CHANNEL_0   // GPIO_NUM_1 on S2

/* INA240 output → ADC (current sense) */
#define PIN_INA240_ADC      ADC1_CHANNEL_1   // GPIO_NUM_2 on S2

#define PIN_POT_ADC_CH      ADC1_CHANNEL_7   // GPIO_NUM_8 on S2
#define PIN_POT_ADC_GPIO    GPIO_NUM_8

#define MASTER_SWITCH_GPIO  PIN_THIRD_WIRE  

#define POT_OVERSAMPLE_N    32              

#define POT_DEADBAND_LOW    82               
#define POT_DEADBAND_HIGH   4013           

/* SPI MISO / MOSI / CLK (shared bus) */
#define PIN_SPI_MOSI        GPIO_NUM_35
#define PIN_SPI_MISO        GPIO_NUM_37
#define PIN_SPI_CLK         GPIO_NUM_36

/* LEDC PWM output channel (same GPIO as GATE_CTRL, muxed in software) */
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_SPEED_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_FREQ_HZ        20000           // 20kHz — above audible, within ISO5451DW rating
#define LEDC_RESOLUTION     LEDC_TIMER_13_BIT   // 13-bit = 8192 counts
#define LEDC_DUTY_50PCT     4096            // 8192 / 2

/* ================================================================
 *  nRF24L01P RF PROTOCOL
 * ================================================================ */
#define NRF_CHANNEL         76
#define NRF_ADDRESS         0xE7E7E7E7E7ULL
#define NRF_PACKET_LEN      6               // bytes 0-5
#define NRF_START_MARKER    0xAA
#define RF_HANDSHAKE_TIMEOUT_MS  30000

/* ================================================================
 *  CELL THRESHOLDS  (configurable via MQTT hub/{id}/cmd/config)
 * ================================================================ */
typedef enum {
    CHEM_LIFEPO4 = 0,
    CHEM_SODIUM_ION
} cell_chemistry_t;

/* LiFePO4 defaults */
#define OV_MV_LIFEPO4       3650
#define UV_MV_LIFEPO4       2500

/* Sodium-Ion defaults */
#define OV_MV_NA_ION        4000
#define UV_MV_NA_ION        2800

/* Balancing */
#define BAL_DELTA_MV        30              // trigger threshold
#define BAL_STOP_MV         15              // stop threshold
#define CELL_COUNT          24

/* Temperature */
#define TEMP_WARN_C         60
#define TEMP_SHUTDOWN_C     80

/* OCP threshold (firmware-visible, HW trips independently) */
#define OC_MA_DEFAULT       40000           // 40A

/* ================================================================
 *  TIMING CONSTANTS
 * ================================================================ */
#define CELL_SCAN_INTERVAL_MS       250
#define TEMP_SCAN_INTERVAL_MS       500
#define TELEMETRY_INTERVAL_MS       5000
#define CELL_REPORT_INTERVAL_MS     30000
#define BLACKBOX_LOG_INTERVAL_MS    60000
#define PREBIAS_REEVAL_INTERVAL_MS  5000
#define INA240_POLL_INTERVAL_MS     100
#define POT_POLL_INTERVAL_MS        50       // 20 Hz  smooth manual response

/* SCP recovery */
#define SCP_RECOVERY_WAIT_MS        10000   // 10 second cool-down
#define SCP_MAX_RETRIES             3       // trips in 60s before permanent fault
#define SCP_RETRY_WINDOW_MS         60000
#define SCP_MONITOR_AFTER_RESTORE_MS 2000
#define SCP_PROBE_SHORTED_PCT       5       // load-side < 5% of nominal = still shorted

/* Third Wire debounce */
#define THIRD_WIRE_DEBOUNCE_MS      200

/* ================================================================
 *  SHARED STATE TYPES
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

/* Central system state struct — protected by g_state_mutex */
typedef struct {
    gate_state_t      gate_state;
    op_mode_t         op_mode;
    scp_state_t       scp_state;
    output_profile_t  output_profile;
    uint8_t           fault_count;
    bool              permanent_fault;
    bool              wifi_connected;
    bool              handshake_done;
    bool              boot_complete;
    uint32_t          pwm_duty_pct;         // 0-100
    int32_t           pack_current_ma;
    uint32_t          pack_voltage_mv;
    uint16_t          cell_mv[CELL_COUNT];
    bool              cell_balancing[CELL_COUNT];
    float             temp_avg_c;
    float             temp_sensors_c[4];
    int8_t            wifi_rssi;
    uint32_t          soc_percent_x10;      // 72.4% stored as 724
    uint32_t          retry_count;
    bool              fault_active;
    /* Thresholds — writable via MQTT */
    uint32_t          ov_mv;
    uint32_t          uv_mv;
    uint32_t          oc_ma;
    uint32_t          bal_delta_mv;
    cell_chemistry_t  chemistry;
} system_state_t;

/* Fault type codes for Black Box */
typedef enum {
    FAULT_NONE              = 0x0000,
    FAULT_CELL_MONITOR_FAIL = 0x0001,
    FAULT_INA240_FAIL       = 0x0002,
    FAULT_SCP_TRIP          = 0x0020,
    FAULT_SCP_RECOVERY      = 0x0021,
    FAULT_SCP_PERMANENT     = 0x0022,
    FAULT_CELL_OV           = 0x0030,
    FAULT_CELL_UV           = 0x0031,
    FAULT_TEMP_WARN         = 0x0040,
    FAULT_TEMP_SHUTDOWN     = 0x0041,
} fault_type_t;

/* Extern declarations — defined in main.c */
extern system_state_t  g_sys;
extern SemaphoreHandle_t g_state_mutex;

/* Config persistency helpers */
void config_load_from_nvs(void);
void config_save_to_nvs(void);
