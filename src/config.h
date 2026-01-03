#ifndef CONFIG_H
#define CONFIG_H

#define SENSOR_READ_INTERVAL_SEC 10      // Sensor reading period (seconds)
#define RAM_BUFFER_SIZE 200              // RAM buffer size before flash write
#define FLASH_WRITE_INTERVAL_SEC 5       // Minimum interval between flash writes (seconds)
#define ADV_CONNECTABLE_INTERVAL_MS 10000 // BLE advertising interval (ms)
                                          // Can be increased to 20000-30000 for maximum power savings

// Flash storage configuration for nRF54L15
#define DATA_PARTITION_OFFSET 0x45000    // Offset from flash0 base (matches overlay)
#define DATA_PARTITION_SIZE 0x7B000      // ~500 KB for data storage (nRF54L15 has 1.5 MB flash)
#define FLASH_PAGE_SIZE 4096             // 4 KB page size for nRF54L15

#endif // CONFIG_H
