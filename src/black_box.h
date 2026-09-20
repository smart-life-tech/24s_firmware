#pragma once
#include "config.h"
#include <stdbool.h>
#include <stdint.h>

typedef bool (*mqtt_publish_fn_t)(const char *topic, const char *payload);

void black_box_init(void);
void black_box_write_boot_event(void);
void black_box_write_telemetry_snapshot(void);
void black_box_write_gate_change(op_mode_t mode);
void black_box_write_fault(fault_type_t fault, int32_t peak_current, uint32_t voltage_mv);
void black_box_write_recovery_attempt(uint32_t retry_count, uint32_t probe_mv);
void black_box_write_cell_threshold(fault_type_t fault, uint8_t cell_idx, uint16_t cell_mv);
void black_box_write_temp_alert(uint8_t sensor_idx, float temp_c);
void black_box_write_temp_sensor_fault(uint8_t sensor_idx);
void black_box_upload_and_clear(mqtt_publish_fn_t publish_fn);
void black_box_task(void *arg);
uint32_t black_box_record_count(void);
