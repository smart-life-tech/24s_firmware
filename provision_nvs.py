#!/usr/bin/env python3
"""
provision_nvs.py — Manufacturing NVS provisioning helper for 24S Smart Hub

Generates an NVS partition image containing the unit-specific values the
production firmware expects in the "24shub" namespace:

  - device_id
  - wifi_ssid
  - wifi_pass
  - mqtt_uri

Usage (PowerShell, cmd, or Bash — one line, or use your shell's own
line-continuation character: ` for PowerShell, ^ for cmd, \ for Bash):
    python provision_nvs.py --device-id 24S-HUB-002 --wifi-ssid "MyNetwork" --wifi-pass "MyPassword" --mqtt-uri "mqtt://192.168.1.50:1883" --out nvs_unit002.bin

This produces a binary NVS image sized to match the "nvs" partition
(0x6000 bytes at offset 0x9000, see partitions.csv). Flash it with:

    esptool.py --chip esp32s2 write_flash 0x9000 nvs_unit002.bin

Requires the ESP-IDF "nvs_partition_gen.py" tool, found at
$IDF_PATH/components/nvs_flash/nvs_partition_gen/nvs_partition_gen.py.
Set IDF_PATH or pass --nvs-gen-tool to point at it directly.
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

NVS_PARTITION_SIZE = "0x6000"
NVS_NAMESPACE = "24shub"


def find_nvs_gen_tool(explicit_path):
    if explicit_path:
        return explicit_path
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidate = os.path.join(
            idf_path, "components", "nvs_flash", "nvs_partition_gen", "nvs_partition_gen.py"
        )
        if os.path.isfile(candidate):
            return candidate
    return None


def write_csv(path, device_id, wifi_ssid, wifi_pass, mqtt_uri):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow([NVS_NAMESPACE, "namespace", "", ""])
        writer.writerow(["device_id", "data", "string", device_id])
        writer.writerow(["wifi_ssid", "data", "string", wifi_ssid])
        writer.writerow(["wifi_pass", "data", "string", wifi_pass])
        writer.writerow(["mqtt_uri", "data", "string", mqtt_uri])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device-id", required=True, help="Unit-specific device ID, e.g. 24S-HUB-002")
    ap.add_argument("--wifi-ssid", required=True)
    ap.add_argument("--wifi-pass", required=True)
    ap.add_argument("--mqtt-uri", required=True, help="e.g. mqtt://192.168.1.50:1883")
    ap.add_argument("--out", required=True, help="Output NVS binary image path")
    ap.add_argument("--nvs-gen-tool", help="Path to nvs_partition_gen.py (default: $IDF_PATH lookup)")
    args = ap.parse_args()

    tool = find_nvs_gen_tool(args.nvs_gen_tool)
    if not tool:
        print("error: nvs_partition_gen.py not found. Set IDF_PATH or pass --nvs-gen-tool.", file=sys.stderr)
        return 1

    with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tmp:
        csv_path = tmp.name
    try:
        write_csv(csv_path, args.device_id, args.wifi_ssid, args.wifi_pass, args.mqtt_uri)
        subprocess.run(
            [sys.executable, tool, "generate", csv_path, args.out, NVS_PARTITION_SIZE],
            check=True,
        )
    finally:
        os.remove(csv_path)

    print(f"Wrote {args.out}. Flash with:")
    print(f"  esptool.py --chip esp32s2 write_flash 0x9000 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
