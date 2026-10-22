# 24S SmartHub Firmware Delivery Package

## Finalized scope

This package is the firmware-to-final-PCB reconciliation for the 24S SmartHub finalized hardware and the existing PWA/MQTT interface.

## Firmware pin assignments (hardware verification pending)

- ESP32-S3-WROOM-1U-N4R2
- GPIO4: main gate / 20 kHz PWM
- GPIO6: FAULT_N
- GPIO47: Third-Wire ON/OFF trigger (debounced; module pad 24 per the supplied netlist mapping)
- J9: pin 1 `third_wire`, pin 2 `-VIN` return (per supplied hardware requirements; schematic/netlist not included here)
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
- GPIO47 Third-Wire trigger with 200 ms debounce
- MQTT/app commands request output ON/OFF; GPIO47 does not select a power level
- Output defaults OFF after boot/reset and requires a fresh activation command
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

## Validation status for this migration

- Firmware GPIO/ADC/SPI definitions reviewed and validated against the current source.
- PlatformIO build: verified; firmware binary was generated successfully and is available at `firmware.bin` in the project root (with PlatformIO output also available under `.pio/build/<environment>/firmware.bin`).
- Automated tests: no project-level automated test suite was present in `src`; build verification and binary generation were completed successfully.
- Physical validation: pending on the assembled PCB, including the actual hardware comparator trip level, 93 V physical cutoff behavior, current capability, thermal behavior, and other hardware-dependent qualification.

Physical validation of the assembled PCB remains separate from this firmware handoff, including validation of the actual hardware comparator trip level, 93 V physical cutoff behavior, current capability, thermal behavior, and other hardware-dependent qualification. The firmware package is considered verified as a generated firmware delivery, but hardware-dependent qualification is still separate and remains pending.

## Outstanding hardware integration

- The PCB has no separate master-switch GPIO. GPIO47 is an ON/OFF trigger only; it does not set the output power level.
- Command priority is: fault/protection shutdown first; MQTT/app OFF disables output; MQTT/app ON enables output. PWM commands are treated only as enable requests (nonzero duty) or OFF (zero); they cannot override GPIO47 or set a separate power level. ON is rejected while a fault is active.
- Retained MQTT gate/PWM enable messages are ignored so a broker-retained ON cannot defeat the reset-to-OFF requirement; retained OFF remains accepted.
- GPIO47 LOW is also the pulled-down/open-wire state. This only indicates the trigger is inactive; it cannot enable the output. Output remains OFF through boot/reset until a fresh MQTT/app activation command.
- The existing SPI assignment uses GPIO39/37/38. Confirm these signals are actually available at the ESP32-S3-WROOM-1U-N4R2 module pads and are not consumed by the fitted flash/PSRAM configuration before manufacturing. No replacement SPI pins were guessed without the PCB/netlist.
- The firmware uses the requested GPIO47 input. The module-pad-24 association and J9 routing are reported from the supplied hardware requirements but could not be cross-checked against the missing KiCad files; verify both against the released module datasheet and netlist.
- The KiCad schematic, PCB, and netlist were not present in the supplied repository. Connector/J9 routing and all component designators/values therefore could not be independently checked.
- Confirm that J9 pin 1 is conditioned to a valid 3.3 V ESP32 input relative to module ground, that inactive/open input is low, and that the J9 pin 2 `-VIN` return is not connected directly to GPIO ground unless the circuit design explicitly makes that safe. GPIO47 is not a high-voltage-tolerant input.
- The firmware source contains no hardware protection-component BOM/values for the Third-Wire interface or comparator threshold. Hardware protection values must be taken from the released schematic/BOM, not inferred from the firmware. The existing software thresholds (including 20 A default overcurrent, 93 V pack cutoff, 60°C warning, and 80°C shutdown) are not substitutes for hardware trip limits.
- RF remote support is absent from this firmware package; no RF command path was available to integrate or verify.
