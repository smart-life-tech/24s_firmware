# 24S SmartHub Firmware Delivery Package

## Finalized scope

This package is the firmware-to-final-PCB reconciliation for the 24S SmartHub finalized hardware and the existing PWA/MQTT interface.

## Firmware pin assignments (hardware verification pending)

- ESP32-S3-WROOM-1U-N4R2
- GPIO4: main gate / 20 kHz PWM
- GPIO6: FAULT_N
- GPIO47: Third-Wire activation input (module pad 24 per the supplied netlist mapping)
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
- GPIO47 Third-Wire hard interlock with inactive pull-down and 200 ms debounce
- Main-output mode requests retained while Third-Wire is inactive; they cannot energize GPIO4 until Third-Wire is active
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

- Firmware GPIO/ADC/SPI definitions reviewed: source-level only; ESP32-S3 module-pad, flash/PSRAM, and PCB-netlist verification is pending because design sources were not supplied.
- PlatformIO build: not run (permission to execute the build was declined).
- Automated tests: none found in `src`.
- Physical validation: pending, including the actual hardware comparator trip level, 93 V physical cutoff behavior, current capability, thermal behavior, and other hardware-dependent qualification.

Physical validation of the assembled PCB remains separate from this firmware handoff, including validation of the actual hardware comparator trip level, 93 V physical cutoff behavior, current capability, thermal behavior, and other hardware-dependent qualification.

## Outstanding hardware integration

- The master-switch GPIO is not assigned in the supplied project. `MASTER_SWITCH_GPIO` remains unassigned; GPIO47 is reserved exclusively for Third-Wire. Firmware supports a separate selector configured so HIGH selects 100% and LOW selects 50%, but that physical selection is not active until the actual selector GPIO is provided and verified against the schematic/netlist.
- Current command priority is: fault/protection shutdown first; Third-Wire LOW always inhibits GPIO4; an explicit MQTT/app OFF also keeps the requested operating mode OFF; with a non-OFF mode request, stable Third-Wire HIGH permits output. This preserves an explicit remote OFF action, but it means a HIGH input does not override a remote OFF. If Third-Wire must force output on regardless of MQTT/app OFF, that conflicts with preserving remote shutdown behavior and needs an explicit priority decision.
- The existing SPI assignment uses GPIO39/37/38. Confirm these signals are actually available at the ESP32-S3-WROOM-1U-N4R2 module pads and are not consumed by the fitted flash/PSRAM configuration before manufacturing. No replacement SPI pins were guessed without the PCB/netlist.
- The firmware uses the requested GPIO47 input. The module-pad-24 association and J9 routing are reported from the supplied hardware requirements but could not be cross-checked against the missing KiCad files; verify both against the released module datasheet and netlist.
- The KiCad schematic, PCB, and netlist were not present in the supplied repository. Connector/J9 routing and all component designators/values therefore could not be independently checked.
- Confirm that J9 pin 1 is conditioned to a valid 3.3 V ESP32 input relative to module ground, that inactive/open input is low, and that the J9 pin 2 `-VIN` return is not connected directly to GPIO ground unless the circuit design explicitly makes that safe. GPIO47 is not a high-voltage-tolerant input.
- The firmware source contains no hardware protection-component BOM/values for the Third-Wire interface or comparator threshold. Hardware protection values must be taken from the released schematic/BOM, not inferred from the firmware. The existing software thresholds (including 20 A default overcurrent, 93 V pack cutoff, 60°C warning, and 80°C shutdown) are not substitutes for hardware trip limits.
- RF remote support is absent from this firmware package; no RF command path was available to integrate or verify.
