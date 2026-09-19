/* hardware_init.h */
#pragma once
#include "esp_err.h"
#include "driver/adc.h"
esp_err_t hardware_init(void);
void gate_hold_off(void);
uint32_t adc_read_mv(adc1_channel_t channel);
void led_blink_fault(void);
void led_blink_amber_1hz(void);

/* ltc6813.h */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
esp_err_t ltc6813_self_test(void);
esp_err_t ltc6813_read_all_cells(uint16_t *cell_mv_out);
void cell_monitor_task(void *arg);

/* ina240.h */
#pragma once
#include "esp_err.h"
#include <stdint.h>
int32_t ina240_read_current_ma(void);
esp_err_t ina240_verify_idle(void);

/* scp_recovery.h */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void scp_recovery_task(void *arg);
void scp_recovery_enable_gate(void);
void scp_clear_permanent_fault(void);

/* third_wire.h */
#pragma once
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void third_wire_task(void *arg);
void third_wire_set_mode_from_mqtt(const char *action);
void third_wire_set_pwm_duty(uint32_t duty_pct);

/* pwm_mode.h */
#pragma once
#include <stdint.h>
void pwm_set_duty(uint32_t duty_pct);
void pwm_start(uint32_t duty_pct);
void pwm_stop(void);
uint32_t pot_read_duty_pct(void);
void     pwm_apply_pot(void);

/* black_box.h */
#pragma once
#include "config.h"
#include <stdint.h>
typedef void (*mqtt_publish_fn_t)(const char *topic, const char *payload);
void black_box_init(void);
void black_box_write_boot_event(void);
void black_box_write_telemetry_snapshot(void);
void black_box_write_gate_change(op_mode_t mode);
void black_box_write_fault(fault_type_t fault, int32_t peak_current, uint32_t voltage_mv);
void black_box_write_recovery_attempt(uint32_t retry_count, uint32_t probe_mv);
void black_box_write_cell_threshold(fault_type_t fault, uint8_t cell_idx, uint16_t cell_mv);
void black_box_write_temp_alert(uint8_t sensor_idx, float temp_c);
void black_box_upload_and_clear(mqtt_publish_fn_t publish_fn);
void black_box_task(void *arg);
uint32_t black_box_record_count(void);

/* mqtt_telemetry.h */
#pragma once
#include "config.h"
#include <stdint.h>
void mqtt_telemetry_task(void *arg);
void mqtt_fault_task(void *arg);
void mqtt_publish_fault(const char *fault_type, int32_t peak_current);
void mqtt_publish_recovery_eta(uint32_t eta_seconds);
void mqtt_publish_gate_state_change(op_mode_t mode);
void mqtt_publish_raw(const char *topic, const char *payload);
void mqtt_handle_command(const char *topic, int topic_len,
                          const char *data, int data_len);

/* wifi_manager.h */
#pragma once
void wifi_manager_start(void);
void wifi_manager_task(void *arg);
