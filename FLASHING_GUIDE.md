# 24S Smart Hub — Firmware Upload & Development Guide
## Software Setup, Hardware Connections, Flashing Procedure

---

## 1. DEVELOPMENT ENVIRONMENT SETUP

### 1.1 Install ESP-IDF (v5.2 recommended)

**Windows (PowerShell as Administrator):**
```powershell
winget install Git.Git Python.Python.3.11
git clone --recursive https://github.com/espressif/esp-idf.git C:\esp\esp-idf
cd C:\esp\esp-idf
git checkout v5.2
.\install.ps1 esp32s2
# Add to PATH:
. C:\esp\esp-idf\export.ps1
```

**Linux / macOS:**
```bash
sudo apt install git cmake ninja-build python3 python3-pip   # Ubuntu
# macOS: brew install cmake ninja python3

git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
git checkout v5.2
./install.sh esp32s2
source export.sh   # add to ~/.bashrc for persistence
```

**Verify:**
```bash
idf.py --version
# Should print: ESP-IDF v5.2.x
```

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

## 3. PROJECT BUILD

```bash
cd 24s_firmware

# Set target (only needed once)
idf.py set-target esp32s2

# Edit device ID before building (or set in sdkconfig):
# Open sdkconfig.defaults and change:
#   CONFIG_DEVICE_ID="24S-HUB-001"    ← unique per unit
#   CONFIG_MQTT_BROKER_URI="mqtt://your-broker:1883"

# Build
idf.py build
```

Build output location: `build/24s_smart_hub.bin`

---

## 4. FLASHING

### 4.1 Full flash (first time — includes bootloader + partition table + app)
```bash
idf.py -p /dev/ttyUSB0 flash    # Linux
idf.py -p COM5 flash             # Windows
idf.py -p /dev/cu.usbmodem* flash  # macOS
```

### 4.2 App only (faster for iterative development)
```bash
idf.py -p /dev/ttyUSB0 app-flash
```

### 4.3 Flash + open monitor immediately
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 4.4 Monitor only (no flash)
```bash
idf.py -p /dev/ttyUSB0 monitor
# Exit: Ctrl+]
```

### 4.5 Erase all flash (factory reset)
```bash
idf.py -p /dev/ttyUSB0 erase-flash
# Then reflash everything:
idf.py -p /dev/ttyUSB0 flash
```

---

## 5. PROVISIONING A NEW UNIT

Each hub unit requires a unique DeviceID stored in eFuse OTP and in NVS.

### 5.1 Write DeviceID to NVS (via provisioning script)
```python
# provisioning/provision_unit.py
# Run after first flash, while connected via USB:

import serial, time

DEVICE_ID = "24S-HUB-001"   # change per unit
BROKER    = "mqtt://192.168.1.100:1883"

# Uses ESP32 console to write NVS values
# (Implement as IDF console component or use nvs_partition_gen.py)
```

### 5.2 Pair keyfob (write DeviceID to nRF24 receiver NVS)
- Physical access to **J8 SPI header** is required
- Connect a programmer to J8
- Flash the pairing tool: `idf.py -p /dev/ttyUSB1 -C tools/pairing flash`
- Enter the 16-bit DeviceID when prompted
- This writes to NVS (NOT eFuse — eFuse write is a separate one-time step)

---

## 6. SERIAL MONITOR OUTPUT GUIDE

During normal boot you should see:
```
=== 24S Smart Hub Boot Sequence ===
[1/8] Peripheral init...
      GPIO / LEDC / SPI / ADC OK
[2/8] Loading NVS...
      NVS OK
[3/8] Black Box init...
      Black Box ready: 0 records stored, next write idx 0
      Boot event logged
[4/8] LTC6813 self-test...
      LTC6813 OK — 24 cells verified
[5/8] INA240 + TLV3501 idle check...
      INA240 idle OK
[6/8] Wi-Fi connect (30s timeout, offline mode if miss)...
[7/8] Waiting for RF handshake (30s)...
      RF handshake confirmed! DeviceID=XXXX Nonce=1
[8/8] Pre-bias sense...
Gate ENABLE — 72V rail going live
=== Boot complete — entering main loop ===
```

### Common boot errors:
| Error message | Cause | Fix |
|---|---|---|
| `LTC6813 self-test FAILED` | Cell tap disconnected or isoSPI wiring error | Check J1/J2 connectors and SM91502ALA isolation transformers |
| `INA240 idle check FAILED` | Shorted sense path or stuck TLV3501 | Check R1 (0.5Ω), U3 (INA240), and U4 (TLV3501) |
| `RF handshake timeout` | Keyfob not paired or out of range | Press keyfob within 30s, check nRF24 SPI wiring |
| `black_box partition not found` | Partition table mismatch | Run `idf.py erase-flash` then reflash with custom partitions.csv |
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
  -m '{"ov_mv":3650,"uv_mv":2500,"oc_ma":40000,"bal_delta_mv":30}'
```

### 7.2 Expected telemetry (every 5 seconds on hub/{id}/telemetry):
```json
{
  "device_id": "24S-HUB-001",
  "timestamp": 1746403200,
  "pack_voltage_mv": 87450,
  "pack_current_ma": 18200,
  "soc_percent": 72.4,
  "temp_avg_c": 30.5,
  "temp_sensors_c": [31.2, 29.8, 30.5],
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
├── CMakeLists.txt          ← Top-level build file
├── partitions.csv          ← Custom partition table (includes black_box partition)
├── sdkconfig.defaults      ← ESP-IDF configuration overrides
└── main/
    ├── CMakeLists.txt      ← Component registration
    ├── config.h            ← ALL pin definitions, thresholds, shared types
    ├── main.c              ← Boot sequence + task spawning (app_main)
    ├── hardware_init.c     ← GPIO, LEDC, SPI, ADC init
    ├── ltc6813.c           ← LTC6813-1 isoSPI driver + cell monitoring task
    ├── scp_recovery.c      ← Hardware SCP + 10-second auto-recovery state machine
    ├── third_wire.c        ← Third Wire GPIO logic + mode state machine
    ├── pwm_mode.c          ← LEDC PWM helpers (20kHz, 13-bit)
    ├── black_box.c         ← Offline ring-buffer logger (512KB flash partition)
    ├── mqtt_telemetry.c    ← Bidirectional MQTT JSON telemetry
    ├── subsystems.c        ← rf_handshake, ina240, pre_bias, config NVS
    └── headers.h           ← All module header declarations
```

---

## 9. DEVELOPMENT WORKFLOW

```bash
# Day-to-day: edit → build → flash → monitor
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor

# Check component sizes:
idf.py size-components

# Run on-chip analysis:
idf.py -p /dev/ttyUSB0 monitor --print_filter="SCP:E,MQTT:I,MAIN:I"

# Generate compile_commands.json for IDE (VS Code / CLion):
idf.py set-target esp32s2
idf.py reconfigure
# Then open folder in VS Code with ESP-IDF extension installed
```

### Recommended VS Code extensions:
- **Espressif IDF** (official) — provides IntelliSense, flash, monitor
- **C/C++** (Microsoft)
- **CMake Tools**

---

## 10. PHASE DELIVERY CHECKLIST

| Phase | Scope | Status |
|---|---|---|
| M2A | Boot, SCP recovery, Third Wire, LTC6813, cell balancing | ✅ Implemented |
| M2B | LEDC PWM 20kHz, nRF24 handshake, pre-bias ADC | ✅ Implemented |
| M2C | MQTT JSON telemetry, Black Box ring buffer, Wi-Fi/4G | ✅ Implemented |
| M2D | PWA Dashboard | Separate deliverable |

---

*Architecture: Syncro Pakistan — Milestone 2 Firmware v1.0*
*Target: ESP32-S2-WROVER-I | IDF v5.2 | FreeRTOS*
