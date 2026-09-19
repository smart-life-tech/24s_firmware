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
