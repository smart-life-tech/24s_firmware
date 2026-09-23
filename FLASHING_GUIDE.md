# 24S Smart Hub — Firmware Upload & Development Guide
## Software Setup, Hardware Connections, Flashing Procedure

---

## 1. DEVELOPMENT ENVIRONMENT SETUP

### 1.1 Install PlatformIO

**Windows, Linux, or macOS:**
```bash
python -m pip install platformio
```

Then verify the CLI is available:
```bash
platformio --version
```

The project is configured to be built and flashed as a PlatformIO ESP-IDF project. This is the supported workflow for the supplied firmware package.

---

## 2. HARDWARE CONNECTIONS FOR FLASHING

The ESP32-S2-WROVER-I is flashed via its built-in USB-OTG port **or** via a USB-to-UART bridge on the UART0 pins.

### 2.1 USB-OTG Method (recommended — no extra hardware)
- Connect a USB cable from your PC to the **USB port on the ESP32-S2 module**
- The S2 has built-in USB-OTG — it enumerates as a CDC serial device
- No external USB-UART adapter needed

**To enter flash mode:**
1. Hold down **GPIO0** (BOOT button) on the module
2. Press and release **EN** (RESET) button
3. Release GPIO0
4. The device is now in download mode

### 2.2 UART Method (if using J8 SPI header or UART pins)
| ESP32-S2 Pin | USB-UART Adapter |
|---|---|
| TXD0 (GPIO43) | RX |
| RXD0 (GPIO44) | TX |
| GND | GND |
| 3.3V | VCC (3.3V only — do NOT use 5V) |

Use **CP2102**, **CH340**, or **FTDI FT232** adapter at 115200 baud.

### 2.3 CRITICAL: Power Isolation During Flashing
> ⚠️ **DO NOT** connect the full 72V battery stack while flashing.
> Power the ESP32-S2 ONLY from the USB port (3.3V logic supply via AMS1117-3.3).
> The gate driver ISO5451DW will remain inactive — all outputs stay at 0V.

---

## 3. PROJECT BUILD (PRIMARY WORKFLOW: PlatformIO)

This package is intended to be built and flashed via PlatformIO using the ESP-IDF framework. This is the supported workflow for the supplied project layout.

```bash
cd 24s_firmware
platformio run
platformio run --target upload
platformio device monitor
```

### PlatformIO build outputs
PlatformIO places binaries under `.pio/build/<environment>/firmware.bin` and the project uses the custom partition table at [partitions.csv](partitions.csv).

> The raw `idf.py` flow is not the preferred path for this package because the project is organized as a PlatformIO ESP-IDF project, not a raw `main/` ESP-IDF app tree.

---

## 4. FLASHING

### 4.1 Build and upload the current firmware
```bash
platformio run --target upload
```

### 4.2 Monitor only
```bash
platformio device monitor
```

### 4.3 Clean build
```bash
platformio run --target clean
platformio run
```

### 4.4 Erase flash (factory reset)
```bash
platformio run --target erase
```

---

## 5. PRODUCTION PROVISIONING

The production firmware does not contain Wi-Fi credentials or a
deployment-specific MQTT broker address.

Before a unit is deployed, the following values must be provisioned
into the `24shub` NVS namespace:

- `device_id`
- `wifi_ssid`
- `wifi_pass`
- `mqtt_uri`

These values are unit/deployment specific and are not compiled into
the production firmware image. If `device_id` is not provisioned, the
firmware falls back to the compiled-in default (`24S-HUB-001`); Wi-Fi
and MQTT startup are refused until their values are provisioned.

The same production firmware binary may therefore be used for
multiple SmartHub units.

### 5.1 Provisioning a unit
A `provision_nvs.py` script is included in the project root to write these
four keys into the `nvs` partition image (or over a live serial connection
via `esptool.py`/`nvs_partition_gen.py`). See the script header for usage.

---

## 6. SERIAL MONITOR OUTPUT GUIDE

During normal boot you should see:
```
=== 24S Smart Hub Boot Sequence ===
[1/5] Peripheral init...
      GPIO / LEDC / SPI / ADC OK
[2/5] Loading NVS...
      NVS OK
[3/5] Black Box init...
      Black Box ready: 0 records stored, next write idx 0
      Boot event logged
[4/5] LTC6811 self-test...
      LTC6811 OK — 24 cells verified
[5/5] INA240 idle check...
      INA240 idle OK
=== Boot complete — entering main loop ===
```

### Common boot errors:
| Error message | Cause | Fix |
|---|---|---|
| `LTC6811 self-test FAILED` | Cell tap disconnected or isoSPI wiring error | Check the LTC6811 daisy-chain wiring and J1/J2 isolation path |
| `INA240 idle check FAILED` | Shorted sense path or stuck current fault path | Check the INA240 current-sense path and load wiring |
| `black_box partition not found` | Partition table mismatch | Verify the custom partitions.csv and rebuild with PlatformIO |
| `NVS needs erase — reflashing` | NVS version change after firmware update | Normal on major updates — config resets to defaults |

---

## 7. MQTT TESTING

### 7.1 Test broker setup (local development)
```bash
# Install Mosquitto
sudo apt install mosquitto mosquitto-clients  # Linux
brew install mosquitto                         # macOS

# Start broker
mosquitto -v

# Subscribe to all hub topics:
mosquitto_sub -h localhost -t "hub/#" -v

# Send test commands:
mosquitto_pub -h localhost -t "hub/24S-HUB-001/cmd/gate" -m '{"action":"ON"}'
mosquitto_pub -h localhost -t "hub/24S-HUB-001/cmd/pwm"  -m '{"duty":75}'
mosquitto_pub -h localhost -t "hub/24S-HUB-001/cmd/reset_fault" -m '{"confirm":true}'
mosquitto_pub -h localhost -t "hub/24S-HUB-001/cmd/config" \
  -m '{"ov_mv":3650,"uv_mv":2500,"oc_ma":20000,"bal_delta_mv":30}'
```

> **Note:** The production firmware limits the software over-current threshold
> to 20 A (20,000 mA). Values above this limit are rejected.

### 7.2 Expected telemetry (every 5 seconds on hub/{id}/telemetry):
```json
{
  "device_id": "24S-HUB-001",
  "timestamp": 1746403200,
  "pack_voltage_mv": 87450,
  "pack_current_ma": 18200,
  "soc_percent": 72.4,
  "temp_avg_c": 30.5,
  "temp_sensors_c": [31.2, 29.8, 30.5, 30.1],
  "gate_state": "PWM_50",
  "pwm_duty_percent": 50,
  "fault_active": false,
  "retry_count": 0,
  "wifi_rssi": -62
}
```

---

## 8. FILE STRUCTURE OVERVIEW

```
24s_firmware/
├── CMakeLists.txt          ← Top-level ESP-IDF project file
├── platformio.ini          ← PlatformIO project configuration (primary build route)
├── partitions.csv          ← Custom partition table (includes black_box partition)
├── sdkconfig.defaults      ← ESP-IDF configuration overrides
├── src/
│   ├── CMakeLists.txt      ← Component registration for the firmware module
│   ├── config.h            ← GPIO map, thresholds, shared state
│   ├── main.c              ← Boot sequence + task spawning
│   ├── hardware_init.c     ← GPIO, LEDC, SPI, ADC init
│   ├── ltc6811.c           ← LTC6811 isoSPI driver + cell monitoring task
│   ├── scp_recovery.c      ← Hardware SCP + recovery state machine
│   ├── third_wire.c        ← GPIO4 gate/PWM control path
│   ├── pwm_mode.c          ← LEDC PWM helpers (20kHz, 11-bit)
│   ├── black_box.c         ← Offline bounded logger with explicit full-buffer reset policy
│   ├── mqtt_telemetry.c    ← Bidirectional MQTT JSON telemetry
│   ├── subsystems.c        ← INA240, NVS, overall state handling
│   └── *.h                 ← Interface declarations for each module
└── FLASHING_GUIDE.md       ← Build and flash instructions
```

---

## 9. DEVELOPMENT WORKFLOW

```bash
# Primary project workflow:
platformio run
platformio run --target upload
platformio device monitor
```

### Recommended VS Code extensions:
- **PlatformIO** (official) — primary project workflow
- **C/C++** (Microsoft)
- **CMake Tools**

---

## 10. PHASE DELIVERY CHECKLIST

| Phase | Scope | Status |
|---|---|---|
| M2A | Boot, SCP recovery, GPIO4 gate/PWM path, LTC6811 cell balancing | ✅ Implemented |
| M2B | LEDC PWM 20kHz, NTC telemetry, ESP-IDF project packaging | ✅ Implemented |
| M2C | MQTT JSON telemetry, Black Box logger, Wi-Fi connectivity | ✅ Implemented |
| M2D | PWA Dashboard | Separate deliverable |

---