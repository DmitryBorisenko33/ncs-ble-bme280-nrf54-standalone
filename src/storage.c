#include "storage.h"
#include "config.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(storage, LOG_LEVEL_DBG);

#define NVS_PARTITION_ID FIXED_PARTITION_ID(nvs_storage)
#define NVS_SECTOR_SIZE 4096
#define NVS_SECTOR_COUNT 2  // Minimum 2 sectors for wear leveling

// NVS keys
#define NVS_KEY_LAST_INDEX 0x01
#define NVS_KEY_LAST_SENT 0x02
#define NVS_KEY_WRAPPED 0x03

// Flash device for data partition
// For nRF54L15, sensor_storage may not be defined, use nvs_storage as fallback
#ifdef USE_PARTITION_MANAGER
// Check if sensor_storage partition exists (PM generates PM_sensor_storage_ID if it exists)
#ifdef PM_sensor_storage_ID
#define DATA_PARTITION_ID FIXED_PARTITION_ID(sensor_storage)
#else
#define DATA_PARTITION_ID FIXED_PARTITION_ID(nvs_storage)
#endif
#else
// For devicetree-based partitions (nRF52832)
#define DATA_PARTITION_ID FIXED_PARTITION_ID(sensor_storage)
#endif
static const struct flash_area *flash_area_data;

// RAM buffer for batching writes
static sensor_record_t ram_buffer[RAM_BUFFER_SIZE];
static uint32_t ram_buffer_count = 0;
static int64_t last_flash_write_time = 0;

// Storage state
static uint32_t current_index = 0;
static uint32_t last_sent_index = 0;
static bool wrapped = false;
static bool initialized = false;

// NVS instance
static struct nvs_fs nvs_fs;

static int flash_write_page(uint32_t page_offset, const void *data, size_t len)
{
    if (!flash_area_data) {
        return -ENODEV;
    }

    // Erase page if needed (simplified - assumes we track erased pages)
    // For ring buffer, we'll erase entire partition on init
    int err = flash_area_erase(flash_area_data, page_offset, FLASH_PAGE_SIZE);
    if (err) {
        return err;
    }

    // Ensure write length is word-aligned; pad with 0xFF if needed
    uint32_t padded_len = (len + 3U) & ~3U; // round up to 4 bytes
    static uint8_t write_buf[FLASH_PAGE_SIZE];
    if (padded_len > FLASH_PAGE_SIZE) {
        return -EINVAL;
    }
    memcpy(write_buf, data, len);
    if (padded_len > len) {
        memset(write_buf + len, 0xFF, padded_len - len);
    }

    err = flash_area_write(flash_area_data, page_offset, write_buf, padded_len);
    return err;
}

static int save_state_to_nvs(void)
{
    int err;
    
    err = nvs_write(&nvs_fs, NVS_KEY_LAST_INDEX, &current_index, sizeof(current_index));
    if (err) {
        return err;
    }
    
    err = nvs_write(&nvs_fs, NVS_KEY_LAST_SENT, &last_sent_index, sizeof(last_sent_index));
    if (err) {
        return err;
    }
    
    uint8_t wrapped_byte = wrapped ? 1 : 0;
    err = nvs_write(&nvs_fs, NVS_KEY_WRAPPED, &wrapped_byte, sizeof(wrapped_byte));
    
    return err;
}

static int load_state_from_nvs(void)
{
    int err;
    size_t len;
    
    len = sizeof(current_index);
    err = nvs_read(&nvs_fs, NVS_KEY_LAST_INDEX, &current_index, len);
    if (err < 0) {
        current_index = 0;
    }
    
    len = sizeof(last_sent_index);
    err = nvs_read(&nvs_fs, NVS_KEY_LAST_SENT, &last_sent_index, len);
    if (err < 0) {
        last_sent_index = 0;
    }
    
    uint8_t wrapped_byte = 0;
    len = sizeof(wrapped_byte);
    err = nvs_read(&nvs_fs, NVS_KEY_WRAPPED, &wrapped_byte, len);
    if (err >= 0) {
        wrapped = (wrapped_byte != 0);
    }
    
    return 0;
}

static int flush_ram_buffer(void)
{
    if (ram_buffer_count == 0) {
        return 0;
    }

    // Calculate how many records fit in a page
    const uint32_t records_per_page = FLASH_PAGE_SIZE / sizeof(sensor_record_t);
    
    // Write records in page-sized chunks
    uint32_t records_written = 0;
    while (records_written < ram_buffer_count) {
        uint32_t records_in_chunk = ram_buffer_count - records_written;
        if (records_in_chunk > records_per_page) {
            records_in_chunk = records_per_page;
        }
        
        // Calculate page offset for current_index
        uint32_t page_index = current_index / records_per_page;
        uint32_t page_offset = page_index * FLASH_PAGE_SIZE;
        
        // Check if we need to wrap
        if (page_offset >= DATA_PARTITION_SIZE) {
            wrapped = true;
            page_index = 0;
            page_offset = 0;
            current_index = 0;
            LOG_WRN("Storage wrapped, resetting to beginning");
        }
        
        // Write page
        int err = flash_write_page(page_offset, 
                                   &ram_buffer[records_written],
                                   records_in_chunk * sizeof(sensor_record_t));
        if (err) {
            LOG_ERR("Flash write failed: %d", err);
            return err;
        }
        
        current_index += records_in_chunk;
        records_written += records_in_chunk;
    }
    
    ram_buffer_count = 0;
    last_flash_write_time = k_uptime_get();
    
    // Save state
    save_state_to_nvs();
    
    LOG_INF("Flushed %u records to flash, total index: %u", records_written, current_index);
    
    return 0;
}

int storage_init(void)
{
    if (initialized) {
        return 0;
    }

    int err;
    
    LOG_INF("Initializing storage...");
    
    // Initialize NVS
    const struct flash_area *nvs_area;
    err = flash_area_open(NVS_PARTITION_ID, &nvs_area);
    if (err) {
        LOG_ERR("Failed to open NVS partition: %d", err);
        return err;
    }
    
    // Get flash device from flash area
    const struct device *flash_dev = flash_area_get_device(nvs_area);
    if (flash_dev == NULL) {
        LOG_ERR("Failed to get flash device");
        flash_area_close(nvs_area);
        return -ENODEV;
    }
    
    nvs_fs.flash_device = flash_dev;
    nvs_fs.offset = nvs_area->fa_off;
    /* Derive sector size from flash geometry */
    struct flash_pages_info info;
    err = flash_get_page_info_by_offs(nvs_fs.flash_device, nvs_fs.offset, &info);
    if (err) {
        LOG_ERR("Failed to get page info for NVS: %d", err);
        flash_area_close(nvs_area);
        return err;
    }
    nvs_fs.sector_size = info.size;
    nvs_fs.sector_count = 2; /* 8 KB total */
    
    /* Erase NVS partition before mount to ensure clean state */
    err = flash_area_erase(nvs_area, 0, nvs_area->fa_size);
    if (err) {
        LOG_ERR("Failed to erase NVS partition: %d", err);
        flash_area_close(nvs_area);
        return err;
    }

    err = nvs_mount(&nvs_fs);
    flash_area_close(nvs_area);
    if (err) {
        LOG_ERR("Failed to mount NVS after erase: %d", err);
        return err;
    }
    LOG_INF("NVS erased and mounted");
    
    // Load state
    load_state_from_nvs();
    LOG_INF("Storage state: index=%u, last_sent=%u, wrapped=%d", 
            current_index, last_sent_index, wrapped);
    
    // Open data partition
    err = flash_area_open(DATA_PARTITION_ID, &flash_area_data);
    if (err) {
        LOG_ERR("Failed to open data partition: %d", err);
        return err;
    }
    
    initialized = true;
    LOG_INF("Storage initialized successfully");
    return 0;
}

int storage_write(const sensor_record_t *record)
{
    if (!initialized) {
        return -ENODEV;
    }
    
    // Add to RAM buffer
    if (ram_buffer_count < RAM_BUFFER_SIZE) {
        ram_buffer[ram_buffer_count++] = *record;
    }
    
    // Flush if buffer is full or time interval passed
    int64_t now = k_uptime_get();
    bool time_to_flush = (now - last_flash_write_time) >= (FLASH_WRITE_INTERVAL_SEC * 1000);
    
    if (ram_buffer_count >= RAM_BUFFER_SIZE || time_to_flush) {
        LOG_DBG("Flushing RAM buffer: count=%u, time_to_flush=%d", 
                ram_buffer_count, time_to_flush);
        return flush_ram_buffer();
    }
    
    return 0;
}

int storage_read(uint32_t index, sensor_record_t *record)
{
    if (!initialized || !record) {
        return -EINVAL;
    }

    /* Read from RAM buffer if index is within buffered (not yet flushed) range */
    if (index >= current_index && index < (current_index + ram_buffer_count)) {
        uint32_t buffer_index = index - current_index;
        if (buffer_index < ram_buffer_count) {
            *record = ram_buffer[buffer_index];
            return 0;
        } else {
            return -EINVAL;
        }
    }

    /* Read from flash if index < current_index */
    if (index >= current_index) {
        /* Not yet written to flash and not in buffer */
        return -EINVAL;
    }

    uint32_t max_count = storage_get_max_count();
    if (index >= max_count) {
        return -EINVAL;
    }

    uint32_t offset = index * sizeof(sensor_record_t);
    return flash_area_read(flash_area_data, offset, record, sizeof(sensor_record_t));
}

uint32_t storage_get_count(void)
{
    if (!initialized) {
        return 0;
    }
    
    // Include records in RAM buffer
    uint32_t total = current_index + ram_buffer_count;
    
    if (wrapped) {
        return storage_get_max_count();
    }
    
    return total;
}

uint32_t storage_get_max_count(void)
{
    return DATA_PARTITION_SIZE / sizeof(sensor_record_t);
}

uint32_t storage_get_last_sent(void)
{
    return last_sent_index;
}

int storage_set_last_sent(uint32_t index)
{
    if (!initialized) {
        return -ENODEV;
    }
    
    last_sent_index = index;
    return save_state_to_nvs();
}

bool storage_is_wrapped(void)
{
    return wrapped;
}

