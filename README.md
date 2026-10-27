# 24S SmartHub Firmware

Firmware for the 24S SmartHub, an ESP32-S3-based battery monitoring and main-output control system. The project uses ESP-IDF through PlatformIO and provides cell-monitoring integration, current and temperature sensing, protection-state handling, MQTT telemetry and commands, and persistent event logging.


## Features

* ESP32-S3 firmware using ESP-IDF and FreeRTOS.
* LTC6811-1-based cell-monitoring driver and 24-cell state model.
* INA240 current-sense input and four NTC temperature inputs.
* Main gate control on GPIO4 using 20 kHz LEDC PWM.
* Software protection and fault-state handling for cell overvoltage and undervoltage, pack voltage, overcurrent, temperature, sensor failures, and short-circuit recovery.
* Wi-Fi connectivity and bidirectional MQTT JSON telemetry and commands.
* Persistent NVS configuration for device ID, Wi-Fi credentials, and MQTT broker URI.
* Flash-backed circular black-box event logging and deferred upload.
* Configurable protection thresholds and battery chemistry selection.

## Hardware target and pin map

Target module specified by the project BOM: **ESP32-S3-WROOM-1U-N4R2**, with 4 MB flash, 2 MB PSRAM, and an external antenna connection.

| Function                         | ESP32-S3 GPIO / setting |
| -------------------------------- | ----------------------- |
| Main gate / PWM output           | GPIO4, 20 kHz           |
| Hardware fault input (`FAULT_N`) | GPIO6                   |
| Third-Wire input                 | GPIO47                  |
| LTC6811 chip select              | GPIO13                  |
| SPI MOSI                         | GPIO39                  |
| SPI MISO                         | GPIO37                  |
| SPI SCK                          | GPIO38                  |
| Potentiometer ADC                | GPIO8 / ADC1 channel 7  |
| INA240 current-sense ADC         | GPIO2 / ADC1 channel 1  |
| NTC 1                            | GPIO3 / ADC1 channel 2  |
| NTC 2                            | GPIO7 / ADC1 channel 6  |
| NTC 3                            | GPIO9 / ADC1 channel 8  |
| NTC 4                            | GPIO10 / ADC1 channel 9 |

**Hardware integration warning:** Confirm these pin assignments against the released schematic, netlist, and ESP32-S3 module datasheet. The firmware archive does not itself establish PCB routing, electrical protection adequacy, or physical validation results. GPIO inputs must not be exposed to pack voltage or other levels outside ESP32-S3 limits.

## Output-control behavior in the current source

The current source snapshot uses GPIO47 as a **power selector**. After a stable reading for 200 ms, LOW selects 50% PWM and HIGH selects 100% output.

MQTT/app commands request output ON or OFF. A nonzero PWM command acts as an ON request, while zero acts as OFF. The output defaults OFF at boot/reset and requires a fresh activation request. Blocking faults inhibit output activation.

This is the behavior presently in the source and is **not yet aligned with the latest requested control behavior**. The requested update is for the Third Wire (GPIO47) and app/MQTT to be ON/OFF triggers only, with GPIO47 no longer choosing the power level.

RF remote support is not included in this firmware snapshot. A separate physical master-switch input is not currently configured in firmware.

## Repository layout

```text
.
├── platformio.ini          # PlatformIO environment
├── sdkconfig.defaults      # ESP-IDF defaults
├── partitions.csv          # Custom flash partition table
├── provision_nvs.py        # Generate device-specific NVS image
├── FLASHING_GUIDE.md       # Detailed setup/flashing guide
├── DELIVERY_MANIFEST.md    # Scope, pin map, and known integration gaps
├── mqtt_test.py            # MQTT test helper
└── src/
    ├── main.c              # Boot sequence and task startup
    ├── config.h            # Shared configuration, pins, thresholds, state
    ├── hardware_init.c     # GPIO, LEDC, SPI, and ADC setup
    ├── ltc6811.c           # LTC6811 cell-monitor driver
    ├── ina240.c            # Current-sense checking
    ├── third_wire.c        # Output arbitration and Third-Wire handling
    ├── pwm_mode.c          # PWM and potentiometer ADC helpers
    ├── scp_recovery.c      # Short-circuit recovery state machine
    ├── mqtt_telemetry.c    # MQTT command parsing and telemetry publishing
    ├── wifi_manager.c      # Wi-Fi management
    ├── subsystems.c        # NVS configuration and monitoring logic
    └── black_box.c         # Persistent event logging
```

## Development setup

### Requirements

* PlatformIO Core or the PlatformIO IDE extension.
* Espressif 32 platform and ESP-IDF toolchain managed by PlatformIO.
* USB connection through the board's routed native USB Serial/JTAG interface or a compatible USB-to-UART adapter.

Open the project directory containing `platformio.ini`, then use:

```bash
pio run
pio device monitor -b 115200
```

To upload, first ensure that the target-board and memory configuration match the actual hardware:

```bash
pio run -t upload
```

### Important memory configuration note

The checked-in `platformio.ini` currently selects `esp32-s3-devkitc-1` and sets `board_build.flash_size = 8MB`. `sdkconfig.defaults` also sets 8 MB flash.

These settings do not match the project's specified ESP32-S3-WROOM-1U-N4R2 module, which has 4 MB flash and 2 MB PSRAM.

Select a suitable 4 MB / 2 MB PSRAM PlatformIO profile or define a dedicated board manifest for the custom 24S PCB. Align the generated ESP-IDF configuration and bootloader with the actual module.

The supplied `partitions.csv` partitions fit within 4 MB. Keep the current partition layout unless a separately reviewed partition change is required.

## Device provisioning (NVS)

The firmware expects these values in the `24shub` NVS namespace:

* `device_id`
* `wifi_ssid`
* `wifi_pass`
* `mqtt_uri`

Generate a device-specific NVS image using the included helper and an ESP-IDF NVS partition generator available in the selected Python environment:

```bash
python3 provision_nvs.py \
  --device-id "24S-HUB-001" \
  --wifi-ssid "YOUR_WIFI_SSID" \
  --wifi-pass "YOUR_WIFI_PASSWORD" \
  --mqtt-uri "mqtt://BROKER_HOST:1883" \
  --out nvs_24s.bin \
  --nvs-gen-tool "$IDF_PATH/components/nvs_flash/nvs_partition_gen/nvs_partition_gen.py"
```

The output image is sized for the `nvs` partition (`0x6000` bytes at offset `0x9000`).

Once the target's flash layout is confirmed, the NVS image can be written using:

```bash
esptool.py --chip esp32s3 write_flash 0x9000 nvs_24s.bin
```

Do not commit Wi-Fi credentials, broker credentials, or device-specific NVS images to a public repository. Use placeholders in examples and provision real values per device.

## MQTT interface

Topics use the device ID as part of the topic path.

| Topic                             | Purpose                                                           |
| --------------------------------- | ----------------------------------------------------------------- |
| `hub/<device_id>/telemetry`       | Periodic state snapshot, approximately every 5 seconds            |
| `hub/<device_id>/cells`           | Cell-monitoring data                                              |
| `hub/<device_id>/faults`          | Fault and recovery events                                         |
| `hub/<device_id>/cmd/gate`        | Output enable/disable request                                     |
| `hub/<device_id>/cmd/pwm`         | Currently maps zero duty to OFF and nonzero duty to an ON request |
| `hub/<device_id>/cmd/reset_fault` | Request to clear a permanent fault, subject to firmware checks    |
| `hub/<device_id>/cmd/config`      | Update supported protection thresholds and battery chemistry      |

In the current source, the PWM command does not set an arbitrary output duty. GPIO47 selects the output level instead.

### Example commands

For a broker on the development network:

```bash
mosquitto_pub -h BROKER_HOST \
  -t "hub/24S-HUB-001/cmd/gate" \
  -m '{"action":"ON"}'

mosquitto_pub -h BROKER_HOST \
  -t "hub/24S-HUB-001/cmd/gate" \
  -m '{"action":"OFF"}'

mosquitto_pub -h BROKER_HOST \
  -t "hub/24S-HUB-001/cmd/pwm" \
  -m '{"duty":0}'

mosquitto_pub -h BROKER_HOST \
  -t "hub/24S-HUB-001/cmd/reset_fault" \
  -m '{"confirm":true}'

mosquitto_pub -h BROKER_HOST \
  -t "hub/24S-HUB-001/cmd/config" \
  -m '{"ov_mv":3650,"uv_mv":2500,"oc_ma":20000,"bal_delta_mv":30,"chemistry":"LIFEPO4"}'
```

The firmware ignores retained commands that would activate the output, while allowing retained OFF commands. Validate command payloads and state transitions against the current source before integrating with an app or production broker.

## Protection and safe-start behavior

* The main output is commanded OFF at startup/reset.
* Normal startup requires successful hardware initialization and LTC6811 self-test, followed by the INA240 idle check.
* A failed cell-monitor self-test or current-sense idle check prevents normal operation and leaves the system in safe standby.
* Output activation is inhibited while a blocking fault is active.
* Short-circuit recovery is handled separately from unrelated protection faults.

Do not bypass these safeguards in production. Any bench-test bypass for a disconnected sensor or missing subsystem must be isolated to an explicit development/test configuration and excluded from production builds.

## Current limitations and validation

* The latest requested Third-Wire behavior (ON/OFF only) still needs to be implemented; the current source uses GPIO47 for power selection.
* The checked-in PlatformIO/ESP-IDF settings still specify 8 MB flash and need to be reconciled with the specified 4 MB / 2 MB PSRAM module.
* The source package alone does not verify PCB routing or the electrical suitability of the Third-Wire protection network.
* RF remote support is absent from this firmware snapshot.
* A physical master-switch input is not currently configured in the firmware.
* Hardware-dependent tests remain necessary for LTC6811 communication, current sensing, comparator trip behavior, pack cutoff behavior, thermal operation, and output-stage performance.
* Default software thresholds are not substitutes for hardware protection thresholds.

## License and contributions

Add the project's approved license and contribution terms before publishing the repository publicly. Do not publish confidential client material, production credentials, device-specific NVS images, or unreleased manufacturing data without authorization.
