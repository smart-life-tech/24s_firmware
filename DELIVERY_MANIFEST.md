# 24S SmartHub Firmware Delivery Package

## Finalized scope

This package is the firmware-to-final-PCB reconciliation for the 24S SmartHub finalized hardware and the existing PWA/MQTT interface.

## Final hardware mapping

- ESP32-S2-WROVER-I-N4R2
- GPIO4: main gate / 20 kHz PWM
- GPIO6: FAULT_N
- SPI: MOSI GPIO39, MISO GPIO37, SCK GPIO38, CS GPIO13
- INA240 ADC: GPIO2 / ADC1_CH1
- NTC1: GPIO3 / ADC1_CH2
- NTC2: GPIO7 / ADC1_CH6
- NTC3: GPIO9 / ADC1_CH8
- NTC4: GPIO10 / ADC1_CH9

## Reconciliation included

- LTC6811-1 two-device / 24-cell monitoring
- Four-NTC telemetry
- GPIO4 gate control and 20 kHz PWM
- Removal of legacy GPIO5 physical Third-Wire path
- Removal of PREBIAS and nRF24/RF handshake dependencies
- 93 V software pack cutoff
- Hardware-fault observation and recovery state machine
- Protection-state separation so SCP recovery does not clear unrelated faults
- Configurable OV/UV/OC/balancing thresholds
- Dual-chemistry threshold selection through configuration
- PWA-compatible MQTT topics and command interface
- Stable fault JSON schema
- Black-box flash-safe circular record storage and deferred upload
- Fault-event cell/sensor indices in black-box export
- SNTP startup after Wi-Fi association
- Safer initialization/error handling

## Validation performed on the source package

- C-source delimiter/syntax balance check: PASS
- C-file syntax-only compiler pass with ESP-IDF API stubs: PASS
- Cross-file function declaration/definition check: PASS
- Final GPIO/ADC/SPI mapping consistency: PASS
- Legacy nRF24/PREBIAS/GPIO5 functional references: PASS (none present)
- 4-NTC telemetry presence: PASS
- MQTT command topic compatibility: PASS
- Black-box record-size assertion: PASS
- Partition/record-area boundary checks: PASS

Physical validation of the assembled PCB remains separate from this firmware handoff, including validation of the actual hardware comparator trip level, 93 V physical cutoff behavior, current capability, thermal behavior, and other hardware-dependent qualification.
