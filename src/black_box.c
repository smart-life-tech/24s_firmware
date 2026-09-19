/**
 * black_box.c — Offline Data Logger (Section 10)
 *
 * Writes to a 512KB NVS partition formatted as a FIFO ring buffer.
 * Fixed 64-byte records. Max 500 records (~8 hours at 60s intervals).
 * Runs from boot — no Wi-Fi required.
 * On MQTT reconnect, uploads full buffer as JSON array to hub/{id}/blackbox.
 */

#include "black_box.h"
#include "config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "BLACK_BOX";

/* ----------------------------------------------------------------
 *  Record layout — 64 bytes exactly (matches Section 10.1)
 * ---------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;           // bytes 0-3   Unix timestamp
    uint16_t event_type;          // bytes 4-5   event code
    uint32_t pack_voltage_mv;     // bytes 6-9
    int32_t  pack_current_ma;     // bytes 10-13 signed
    uint8_t  fault_flags;         // byte 14
    uint16_t cell_mv[24];         // bytes 15-62 (24 × 2 = 48 bytes)
    uint8_t  avg_temp_c;          // byte 63
} bb_record_t;

_Static_assert(sizeof(bb_record_t) == 64, "bb_record_t must be 64 bytes");

/* ----------------------------------------------------------------
 *  Ring buffer header — stored at start of partition
 * ---------------------------------------------------------------- */
#define BB_PARTITION_LABEL   "black_box"
#define BB_MAX_RECORDS       500
#define BB_RECORD_SIZE       64
#define BB_HEADER_SIZE       16   // magic(4) + write_idx(4) + count(4) + crc(4)
#define BB_MAGIC             0xBB24BBBB
#define BB_RECORD_DATA_OFFSET_DEFAULT 4096

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_idx;    // next write position (0-based)
    uint32_t count;        // total records stored (saturates at BB_MAX_RECORDS)
    uint32_t crc;          // simple XOR checksum of above 3 fields
} bb_header_t;

static const esp_partition_t *bb_partition = NULL;
static bb_header_t            bb_hdr;
static SemaphoreHandle_t      bb_mutex;

/* ----------------------------------------------------------------
 *  CRC helper
 * ---------------------------------------------------------------- */
static uint32_t header_crc(const bb_header_t *h)
{
    return h->magic ^ h->write_idx ^ h->count;
}

/* ----------------------------------------------------------------
 *  Partition access helpers
 * ---------------------------------------------------------------- */
static esp_err_t hdr_read(void)
{
    return esp_partition_read(bb_partition, 0, &bb_hdr, sizeof(bb_hdr));
}

static esp_err_t hdr_write(void)
{
    bb_hdr.crc = header_crc(&bb_hdr);
    uint32_t erase_size = esp_partition_get_erase_size(bb_partition, 0);
    if (erase_size > 0U) {
        esp_partition_erase_range(bb_partition, 0, erase_size);
    }
    return esp_partition_write(bb_partition, 0, &bb_hdr, sizeof(bb_hdr));
}

static uint32_t record_offset(uint32_t idx)
{
    uint32_t erase_size = esp_partition_get_erase_size(bb_partition, 0);
    uint32_t start = (erase_size > 0U) ? erase_size : BB_RECORD_DATA_OFFSET_DEFAULT;
    return start + (idx % BB_MAX_RECORDS) * BB_RECORD_SIZE;
}

/* ----------------------------------------------------------------
 *  Init
 * ---------------------------------------------------------------- */
void black_box_init(void)
{
    bb_mutex = xSemaphoreCreateMutex();

    bb_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY,
                                            BB_PARTITION_LABEL);
    if (!bb_partition) {
        ESP_LOGE(TAG, "black_box partition not found! Check partition table.");
        return;
    }

    /* Read and validate header */
    hdr_read();
    if (bb_hdr.magic != BB_MAGIC ||
        bb_hdr.crc   != header_crc(&bb_hdr)) {
        ESP_LOGW(TAG, "Invalid header — formatting partition");
        esp_partition_erase_range(bb_partition, 0, bb_partition->size);
        bb_hdr.magic     = BB_MAGIC;
        bb_hdr.write_idx = 0;
        bb_hdr.count     = 0;
        hdr_write();
    }

    ESP_LOGI(TAG, "Black Box ready: %d records stored, next write idx %d",
             bb_hdr.count, bb_hdr.write_idx);
}

/* ----------------------------------------------------------------
 *  Core write function
 * ---------------------------------------------------------------- */
static void bb_write_record(uint16_t event_type, int32_t current_ma,
                             uint32_t voltage_mv, uint8_t fault_flags,
                             const uint16_t *cell_mv, uint8_t temp_c)
{
    if (!bb_partition) return;

    xSemaphoreTake(bb_mutex, portMAX_DELAY);

    bb_record_t rec = {0};
    rec.timestamp       = (uint32_t)(time(NULL));
    rec.event_type      = event_type;
    rec.pack_voltage_mv = voltage_mv;
    rec.pack_current_ma = current_ma;
    rec.fault_flags     = fault_flags;
    rec.avg_temp_c      = temp_c;

    if (cell_mv) {
        memcpy(rec.cell_mv, cell_mv, CELL_COUNT * sizeof(uint16_t));
    } else {
        /* Fill from global state */
        memcpy(rec.cell_mv, g_sys.cell_mv, CELL_COUNT * sizeof(uint16_t));
    }

    uint32_t offset = record_offset(bb_hdr.write_idx);

    /*
     * Keep the header in the first erase sector and the ring records in the
     * subsequent sector(s). This avoids erasing previously logged records when
     * a header field is updated.
     */
    esp_partition_write(bb_partition, offset, &rec, sizeof(rec));

    bb_hdr.write_idx = (bb_hdr.write_idx + 1) % BB_MAX_RECORDS;
    if (bb_hdr.count < BB_MAX_RECORDS) bb_hdr.count++;
    hdr_write();

    xSemaphoreGive(bb_mutex);

    ESP_LOGD(TAG, "Record [0x%04X] written at idx %d", event_type,
             (bb_hdr.write_idx - 1 + BB_MAX_RECORDS) % BB_MAX_RECORDS);
}

/* ----------------------------------------------------------------
 *  Public write helpers
 * ---------------------------------------------------------------- */

void black_box_write_boot_event(void)
{
    bb_write_record(0x0050, 0, g_sys.pack_voltage_mv, 0, NULL,
                    (uint8_t)g_sys.temp_avg_c);
    ESP_LOGI(TAG, "Boot event logged");
}

void black_box_write_telemetry_snapshot(void)
{
    bb_write_record(0x0001,
                    g_sys.pack_current_ma,
                    g_sys.pack_voltage_mv,
                    0, NULL,
                    (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_gate_change(op_mode_t mode)
{
    bb_write_record(0x0010, 0, g_sys.pack_voltage_mv,
                    (uint8_t)mode, NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_fault(fault_type_t fault, int32_t peak_current,
                            uint32_t voltage_mv)
{
    uint16_t ev;
    switch (fault) {
    case FAULT_SCP_TRIP:       ev = 0x0020; break;
    case FAULT_SCP_RECOVERY:   ev = 0x0021; break;
    case FAULT_SCP_PERMANENT:  ev = 0x0022; break;
    case FAULT_CELL_OV:        ev = 0x0030; break;
    case FAULT_CELL_UV:        ev = 0x0031; break;
    case FAULT_PACK_OV:        ev = 0x0032; break;
    case FAULT_TEMP_WARN:      ev = 0x0040; break;
    case FAULT_TEMP_SHUTDOWN:  ev = 0x0041; break;
    case FAULT_INA240_FAIL:    ev = 0x0002; break;
    default:                   ev = 0x0000; break;
    }
    bb_write_record(ev, peak_current, voltage_mv,
                    (uint8_t)fault, NULL, (uint8_t)g_sys.temp_avg_c);
    ESP_LOGW(TAG, "Fault [0x%04X] logged, peak %d mA", ev, peak_current);
}

void black_box_write_recovery_attempt(uint32_t retry_count, uint32_t probe_mv)
{
    /* Store retry count in current_ma field, probe_mv in voltage_mv */
    bb_write_record(0x0021, (int32_t)retry_count, probe_mv,
                    0, NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_cell_threshold(fault_type_t fault, uint8_t cell_idx,
                                     uint16_t cell_mv)
{
    uint16_t cells[CELL_COUNT];
    memcpy(cells, g_sys.cell_mv, sizeof(cells));
    bb_write_record(0x0030, cell_idx, cell_mv, (uint8_t)fault,
                    cells, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_temp_alert(uint8_t sensor_idx, float temp_c)
{
    uint16_t event_code = (temp_c >= TEMP_SHUTDOWN_C) ? 0x0041 : 0x0040;
    bb_write_record(event_code, sensor_idx, (uint32_t)(temp_c * 10),
                    0, NULL, (uint8_t)temp_c);
}

/* ----------------------------------------------------------------
 *  Upload buffer as JSON to MQTT on reconnect
 * ---------------------------------------------------------------- */
static const char *bb_event_name(uint16_t event_type)
{
    switch (event_type) {
    case 0x0001: return "TELEMETRY_SNAPSHOT";
    case 0x0002: return "INA240_FAIL";
    case 0x0010: return "GATE_CHANGE";
    case 0x0020: return "SCP_TRIP";
    case 0x0021: return "SCP_RECOVERY";
    case 0x0022: return "SCP_PERMANENT";
    case 0x0030: return "CELL_OV";
    case 0x0031: return "CELL_UV";
    case 0x0032: return "PACK_OV";
    case 0x0040: return "TEMP_WARN";
    case 0x0041: return "TEMP_SHUTDOWN";
    case 0x0050: return "BOOT";
    default:     return "UNKNOWN";
    }
}

void black_box_upload_and_clear(mqtt_publish_fn_t publish_fn)
{
    if (!bb_partition || bb_hdr.count == 0) return;

    xSemaphoreTake(bb_mutex, portMAX_DELAY);

    uint32_t count = bb_hdr.count;
    uint32_t start = (bb_hdr.count < BB_MAX_RECORDS) ?
                     0 : bb_hdr.write_idx;

    ESP_LOGI(TAG, "Uploading %d records to MQTT...", count);

    char json_buf[256];
    char topic[64];
    snprintf(topic, sizeof(topic), "hub/%s/blackbox", CONFIG_DEVICE_ID);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % BB_MAX_RECORDS;
        uint32_t offset = record_offset(idx);

        bb_record_t rec;
        esp_partition_read(bb_partition, offset, &rec, sizeof(rec));

        snprintf(json_buf, sizeof(json_buf),
            "{\"timestamp\":%u,\"fault_type\":\"%s\",\"peak_current_ma\":%d,"
            "\"pack_voltage_mv\":%u,\"event_type\":\"0x%04X\"}",
            rec.timestamp, bb_event_name(rec.event_type), rec.pack_current_ma,
            rec.pack_voltage_mv, rec.event_type);

        publish_fn(topic, json_buf);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_partition_erase_range(bb_partition, 0, bb_partition->size);
    bb_hdr.write_idx = 0;
    bb_hdr.count     = 0;
    hdr_write();

    xSemaphoreGive(bb_mutex);
    ESP_LOGI(TAG, "Black Box upload complete — buffer cleared");
}

/* ----------------------------------------------------------------
 *  Periodic task: write telemetry snapshot every 60s
 * ---------------------------------------------------------------- */
void black_box_task(void *arg)
{
    ESP_LOGI(TAG, "Black Box task running");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BLACKBOX_LOG_INTERVAL_MS));
        black_box_write_telemetry_snapshot();
    }
}

/* ----------------------------------------------------------------
 *  Query: how many records are stored
 * ---------------------------------------------------------------- */
uint32_t black_box_record_count(void)
{
    return bb_hdr.count;
}
