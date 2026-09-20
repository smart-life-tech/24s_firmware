/**
 * black_box.c — Offline Data Logger
 *
 * 64-byte records stored in a flash-safe circular log. The record area is
 * divided into 4 KiB sectors (64 records/sector); a sector is erased only when
 * the ring is about to reuse it. Header metadata is journaled in two alternating
 * sectors. Upload runs in a worker task so the MQTT callback is never blocked.
 */

#include "black_box.h"
#include "config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "BLACK_BOX";

#ifndef CONFIG_DEVICE_ID
#define CONFIG_DEVICE_ID "24S-HUB-001"
#endif

#define BB_PARTITION_LABEL          "black_box"
#define BB_MAX_RECORDS              500U
#define BB_RECORD_SIZE              64U
#define BB_HEADER_SLOT_COUNT        2U
#define BB_MAGIC                    0xBB24BBBBU
#define BB_HEADER_SLOT_SIZE         4096U
#define BB_RECORDS_PER_SECTOR       64U
#define BB_RECORD_SECTOR_COUNT      ((BB_MAX_RECORDS + BB_RECORDS_PER_SECTOR - 1U) / BB_RECORDS_PER_SECTOR)
#define BB_RECORD_DATA_OFFSET       (BB_HEADER_SLOT_COUNT * BB_HEADER_SLOT_SIZE)
#define BB_RECORD_DATA_SIZE         (BB_RECORD_SECTOR_COUNT * BB_HEADER_SLOT_SIZE)
#define BB_UPLOAD_QUEUE_LEN         2U

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint16_t event_type;
    uint32_t pack_voltage_mv;
    int32_t  pack_current_ma;
    uint8_t  event_aux;
    uint16_t cell_mv[24];
    uint8_t  avg_temp_c;
} bb_record_t;

_Static_assert(sizeof(bb_record_t) == BB_RECORD_SIZE, "bb_record_t must be 64 bytes");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t write_idx;
    uint32_t count;
    uint32_t crc;
} bb_header_t;

static const esp_partition_t *bb_partition = NULL;
static bb_header_t bb_hdr;
static uint32_t bb_hdr_slot = 0U;
static SemaphoreHandle_t bb_mutex = NULL;
static QueueHandle_t bb_upload_queue = NULL;
static mqtt_publish_fn_t bb_publish_fn = NULL;

static uint32_t header_crc(const bb_header_t *h)
{
    return h->magic ^ h->sequence ^ h->write_idx ^ h->count;
}

static uint32_t header_slot_offset(uint32_t slot)
{
    uint32_t erase_size = (bb_partition && bb_partition->erase_size != 0U)
        ? bb_partition->erase_size : BB_HEADER_SLOT_SIZE;
    return slot * erase_size;
}

static uint32_t record_sector_offset(uint32_t sector)
{
    return BB_RECORD_DATA_OFFSET + (sector * BB_HEADER_SLOT_SIZE);
}

static uint32_t record_offset(uint32_t idx)
{
    return BB_RECORD_DATA_OFFSET + ((idx % BB_MAX_RECORDS) * BB_RECORD_SIZE);
}

static esp_err_t hdr_write(void)
{
    if (!bb_partition) return ESP_ERR_INVALID_STATE;

    bb_hdr.crc = header_crc(&bb_hdr);
    uint32_t slot = bb_hdr_slot;
    uint32_t offset = header_slot_offset(slot);
    uint32_t erase_size = (bb_partition->erase_size != 0U)
        ? bb_partition->erase_size : BB_HEADER_SLOT_SIZE;

    if ((offset % erase_size) != 0U || (offset + erase_size) > bb_partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_erase_range(bb_partition, offset, erase_size);
    if (err != ESP_OK) return err;

    err = esp_partition_write(bb_partition, offset, &bb_hdr, sizeof(bb_hdr));
    if (err == ESP_OK) {
        bb_hdr_slot = (slot + 1U) % BB_HEADER_SLOT_COUNT;
    }
    return err;
}

static esp_err_t hdr_read(void)
{
    bb_header_t a = {0};
    bb_header_t b = {0};
    esp_err_t ea = esp_partition_read(bb_partition, header_slot_offset(0), &a, sizeof(a));
    esp_err_t eb = esp_partition_read(bb_partition, header_slot_offset(1), &b, sizeof(b));

    if (ea != ESP_OK && eb != ESP_OK) return ESP_FAIL;

    bool va = (a.magic == BB_MAGIC && a.crc == header_crc(&a) && a.write_idx < BB_MAX_RECORDS && a.count <= BB_MAX_RECORDS);
    bool vb = (b.magic == BB_MAGIC && b.crc == header_crc(&b) && b.write_idx < BB_MAX_RECORDS && b.count <= BB_MAX_RECORDS);

    if (!va && !vb) {
        memset(&bb_hdr, 0, sizeof(bb_hdr));
        bb_hdr.magic = BB_MAGIC;
        bb_hdr_slot = 0U;
        return ESP_OK;
    }

    if (va && (!vb || a.sequence >= b.sequence)) {
        bb_hdr = a;
        bb_hdr_slot = 0U;
    } else {
        bb_hdr = b;
        bb_hdr_slot = 1U;
    }
    return ESP_OK;
}

static esp_err_t erase_record_sector(uint32_t sector)
{
    if (!bb_partition || sector >= BB_RECORD_SECTOR_COUNT) return ESP_ERR_INVALID_ARG;
    return esp_partition_erase_range(bb_partition, record_sector_offset(sector), BB_HEADER_SLOT_SIZE);
}

static esp_err_t erase_all_record_sectors(void)
{
    if (!bb_partition) return ESP_ERR_INVALID_STATE;
    for (uint32_t s = 0; s < BB_RECORD_SECTOR_COUNT; s++) {
        esp_err_t err = erase_record_sector(s);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static const char *bb_event_name(uint16_t event_type)
{
    switch (event_type) {
    case 0x0001: return "TELEMETRY_SNAPSHOT";
    case 0x0002: return "INA240_FAIL";
    case 0x0010: return "GATE_CHANGE";
    case 0x0020: return "SCP_TRIP";
    case 0x0021: return "SCP_RECOVERY";
    case 0x0022: return "SCP_PERMANENT";
    case 0x0023: return "OVERCURRENT";
    case 0x0030: return "CELL_OV";
    case 0x0031: return "CELL_UV";
    case 0x0032: return "PACK_OV";
    case 0x0040: return "TEMP_WARN";
    case 0x0041: return "TEMP_SHUTDOWN";
    case 0x0042: return "TEMP_SENSOR_FAIL";
    case 0x0050: return "BOOT";
    default:     return "UNKNOWN";
    }
}

static void bb_upload_task(void *arg)
{
    (void)arg;
    uint8_t token;

    for (;;) {
        if (xQueueReceive(bb_upload_queue, &token, portMAX_DELAY) != pdTRUE) continue;
        (void)token;
        if (!bb_partition || !bb_publish_fn) continue;

        xSemaphoreTake(bb_mutex, portMAX_DELAY);
        uint32_t count = bb_hdr.count;
        uint32_t start = (count < BB_MAX_RECORDS) ? 0U : bb_hdr.write_idx;
        uint32_t sequence_before = bb_hdr.sequence;
        bb_record_t *records = NULL;

        if (count > 0U) records = (bb_record_t *)malloc(count * sizeof(bb_record_t));
        if (count > 0U && !records) {
            xSemaphoreGive(bb_mutex);
            ESP_LOGE(TAG, "Black Box upload allocation failed");
            continue;
        }

        bool read_ok = true;
        for (uint32_t i = 0; i < count; i++) {
            if (esp_partition_read(bb_partition,
                                   record_offset((start + i) % BB_MAX_RECORDS),
                                   &records[i], sizeof(bb_record_t)) != ESP_OK) {
                read_ok = false;
                break;
            }
        }
        xSemaphoreGive(bb_mutex);

        if (!read_ok) {
            free(records);
            ESP_LOGW(TAG, "Black Box record read failed during upload; buffer retained");
            continue;
        }
        if (count == 0U) {
            free(records);
            continue;
        }

        char topic[64];
        snprintf(topic, sizeof(topic), "hub/%s/blackbox", CONFIG_DEVICE_ID);
        bool upload_ok = true;

        for (uint32_t i = 0; i < count; i++) {
            char json[320];
            uint32_t retry_count = 0U;
            uint32_t cell_index = 0U;
            uint32_t sensor_index = 0U;

            if (records[i].event_type == 0x0020U ||
                records[i].event_type == 0x0021U ||
                records[i].event_type == 0x0022U) {
                retry_count = records[i].event_aux;
            }
            if (records[i].event_type == 0x0030U || records[i].event_type == 0x0031U) {
                cell_index = (uint32_t)records[i].event_aux + 1U;
            }
            if (records[i].event_type == 0x0040U ||
                records[i].event_type == 0x0041U ||
                records[i].event_type == 0x0042U) {
                sensor_index = (uint32_t)records[i].event_aux + 1U;
            }

            int n = snprintf(json, sizeof(json),
                "{\"timestamp\":%u,\"fault_type\":\"%s\",\"peak_current_ma\":%d,"
                "\"pack_voltage_mv\":%u,\"event_type\":\"0x%04X\",\"retry_count\":%u,"
                "\"cell_index\":%u,\"sensor_index\":%u}",
                records[i].timestamp,
                bb_event_name(records[i].event_type),
                records[i].pack_current_ma,
                records[i].pack_voltage_mv,
                records[i].event_type,
                retry_count,
                cell_index,
                sensor_index);

            if (n <= 0 || !bb_publish_fn(topic, json)) {
                upload_ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        xSemaphoreTake(bb_mutex, portMAX_DELAY);
        if (upload_ok && bb_hdr.sequence == sequence_before && bb_hdr.count == count) {
            esp_err_t erase_err = erase_all_record_sectors();
            if (erase_err == ESP_OK) {
                bb_hdr.magic = BB_MAGIC;
                bb_hdr.sequence = sequence_before;
                bb_hdr.write_idx = 0U;
                bb_hdr.count = 0U;
                if (hdr_write() != ESP_OK) {
                    ESP_LOGW(TAG, "Black Box header reset failed after upload");
                }
            } else {
                ESP_LOGW(TAG, "Black Box clear erase failed: %s", esp_err_to_name(erase_err));
            }
        }
        xSemaphoreGive(bb_mutex);

        if (!upload_ok) {
            ESP_LOGW(TAG, "Black Box upload incomplete; buffered records retained");
        }
        free(records);
    }
}

void black_box_init(void)
{
    bb_mutex = xSemaphoreCreateMutex();
    bb_upload_queue = xQueueCreate(BB_UPLOAD_QUEUE_LEN, sizeof(uint8_t));
    if (!bb_mutex || !bb_upload_queue) {
        ESP_LOGE(TAG, "Black Box synchronization objects could not be created");
        return;
    }

    bb_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY,
                                            BB_PARTITION_LABEL);
    if (!bb_partition) {
        ESP_LOGE(TAG, "Black Box partition not found");
        return;
    }

    if (bb_partition->erase_size < BB_HEADER_SLOT_SIZE ||
        (BB_RECORD_DATA_OFFSET + BB_RECORD_DATA_SIZE) > bb_partition->size) {
        ESP_LOGE(TAG, "Black Box partition too small: %u bytes", (unsigned)bb_partition->size);
        bb_partition = NULL;
        return;
    }

    bool header_valid = (hdr_read() == ESP_OK &&
                         bb_hdr.magic == BB_MAGIC &&
                         bb_hdr.crc == header_crc(&bb_hdr));
    if (!header_valid) {
        ESP_LOGW(TAG, "Invalid Black Box header; formatting record area");
        if (erase_all_record_sectors() != ESP_OK) {
            ESP_LOGE(TAG, "Black Box record-area erase failed");
            bb_partition = NULL;
            return;
        }
        memset(&bb_hdr, 0, sizeof(bb_hdr));
        bb_hdr.magic = BB_MAGIC;
        bb_hdr_slot = 0U;
        if (hdr_write() != ESP_OK) {
            ESP_LOGE(TAG, "Black Box header write failed");
            bb_partition = NULL;
            return;
        }
    }

    if (xTaskCreate(bb_upload_task, "bb_upload", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Black Box upload task creation failed");
    }

    ESP_LOGI(TAG, "Black Box ready: %u records stored, next idx %u", bb_hdr.count, bb_hdr.write_idx);
}

static void bb_write_record(uint16_t event_type, int32_t current_ma,
                            uint32_t voltage_mv, uint8_t event_aux,
                            const uint16_t *cell_mv, uint8_t temp_c)
{
    if (!bb_partition || !bb_mutex) return;

    xSemaphoreTake(bb_mutex, portMAX_DELAY);

    if (bb_hdr.count >= BB_MAX_RECORDS &&
        (bb_hdr.write_idx % BB_RECORDS_PER_SECTOR) == 0U) {
        if (erase_record_sector(bb_hdr.write_idx / BB_RECORDS_PER_SECTOR) != ESP_OK) {
            ESP_LOGE(TAG, "Black Box sector erase failed at idx %u", bb_hdr.write_idx);
            xSemaphoreGive(bb_mutex);
            return;
        }
    }

    bb_record_t rec = {0};
    rec.timestamp = (uint32_t)time(NULL);
    rec.event_type = event_type;
    rec.pack_voltage_mv = voltage_mv;
    rec.pack_current_ma = current_ma;
    rec.event_aux = event_aux;
    rec.avg_temp_c = temp_c;
    if (cell_mv) memcpy(rec.cell_mv, cell_mv, sizeof(rec.cell_mv));
    else memcpy(rec.cell_mv, g_sys.cell_mv, sizeof(rec.cell_mv));

    esp_err_t err = esp_partition_write(bb_partition, record_offset(bb_hdr.write_idx), &rec, sizeof(rec));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Black Box record write failed: %s", esp_err_to_name(err));
        xSemaphoreGive(bb_mutex);
        return;
    }

    bb_hdr.sequence++;
    bb_hdr.write_idx = (bb_hdr.write_idx + 1U) % BB_MAX_RECORDS;
    if (bb_hdr.count < BB_MAX_RECORDS) bb_hdr.count++;

    if ((bb_hdr.sequence % 8U) == 0U ||
        (bb_hdr.write_idx % BB_RECORDS_PER_SECTOR) == 0U ||
        bb_hdr.count == BB_MAX_RECORDS) {
        if (hdr_write() != ESP_OK) {
            ESP_LOGW(TAG, "Black Box header checkpoint failed");
        }
    }

    xSemaphoreGive(bb_mutex);
}

void black_box_write_boot_event(void)
{
    bb_write_record(0x0050U, 0, g_sys.pack_voltage_mv, 0U, NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_telemetry_snapshot(void)
{
    bb_write_record(0x0001U, g_sys.pack_current_ma, g_sys.pack_voltage_mv, 0U,
                    NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_gate_change(op_mode_t mode)
{
    bb_write_record(0x0010U, 0, g_sys.pack_voltage_mv, (uint8_t)mode,
                    NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_fault(fault_type_t fault, int32_t peak_current, uint32_t voltage_mv)
{
    uint16_t ev = 0x0000U;
    uint8_t aux = 0U;
    switch (fault) {
    case FAULT_SCP_TRIP:      ev = 0x0020U; aux = (uint8_t)(g_sys.retry_count & 0xFFU); break;
    case FAULT_SCP_RECOVERY:  ev = 0x0021U; aux = (uint8_t)(g_sys.retry_count & 0xFFU); break;
    case FAULT_SCP_PERMANENT: ev = 0x0022U; aux = (uint8_t)(g_sys.retry_count & 0xFFU); break;
    case FAULT_OVERCURRENT:    ev = 0x0023U; break;
    case FAULT_CELL_OV:       ev = 0x0030U; break;
    case FAULT_CELL_UV:       ev = 0x0031U; break;
    case FAULT_PACK_OV:       ev = 0x0032U; break;
    case FAULT_TEMP_WARN:     ev = 0x0040U; break;
    case FAULT_TEMP_SHUTDOWN: ev = 0x0041U; break;
    case FAULT_INA240_FAIL:   ev = 0x0002U; break;
    default:                  break;
    }
    bb_write_record(ev, peak_current, voltage_mv, aux, NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_recovery_attempt(uint32_t retry_count, uint32_t probe_mv)
{
    (void)probe_mv;
    bb_write_record(0x0021U, 0, g_sys.pack_voltage_mv,
                    (uint8_t)(retry_count & 0xFFU), NULL, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_cell_threshold(fault_type_t fault, uint8_t cell_idx, uint16_t cell_mv)
{
    (void)cell_mv;
    uint16_t event_code = (fault == FAULT_CELL_UV) ? 0x0031U : 0x0030U;
    bb_write_record(event_code, 0, g_sys.pack_voltage_mv, cell_idx,
                    g_sys.cell_mv, (uint8_t)g_sys.temp_avg_c);
}

void black_box_write_temp_alert(uint8_t sensor_idx, float temp_c)
{
    uint16_t event_code = (temp_c >= TEMP_SHUTDOWN_C) ? 0x0041U : 0x0040U;
    bb_write_record(event_code, g_sys.pack_current_ma, g_sys.pack_voltage_mv,
                    sensor_idx, NULL, (uint8_t)(temp_c < 0.0f ? 0.0f : temp_c));
}

void black_box_write_temp_sensor_fault(uint8_t sensor_idx)
{
    bb_write_record(0x0042U, g_sys.pack_current_ma, g_sys.pack_voltage_mv,
                    sensor_idx, NULL, 0U);
}

void black_box_upload_and_clear(mqtt_publish_fn_t publish_fn)
{
    if (!bb_upload_queue || !publish_fn) return;
    bb_publish_fn = publish_fn;
    uint8_t token = 1U;
    if (xQueueSend(bb_upload_queue, &token, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Black Box upload request already pending");
    }
}

void black_box_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BLACKBOX_LOG_INTERVAL_MS));
        black_box_write_telemetry_snapshot();
    }
}

uint32_t black_box_record_count(void)
{
    uint32_t count = 0U;
    if (bb_mutex && xSemaphoreTake(bb_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        count = bb_hdr.count;
        xSemaphoreGive(bb_mutex);
    }
    return count;
}
