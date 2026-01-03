#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>

// Compact sensor record format (6 bytes)
typedef struct __attribute__((packed)) {
    int16_t temp_x10;        // Temperature in 0.1 degC units (-3276.8..+3276.7 degC)
    uint16_t press_kpa;      // Pressure in kPa (0..65535 kPa)
    uint8_t  hum_pct;        // Humidity in % (0..255%, saturates at 100%)
    uint8_t  battery_v_x10;  // Battery in 0.1V units (0..25.5V)
} sensor_record_t;

// Initialize storage system
int storage_init(void);

// Write a new record (with automatic overwrite when full)
int storage_write(const sensor_record_t *record);

// Read a record by index
int storage_read(uint32_t index, sensor_record_t *record);

// Get current count of records
uint32_t storage_get_count(void);

// Get maximum capacity
uint32_t storage_get_max_count(void);

// Get last sent index
uint32_t storage_get_last_sent(void);

// Set last sent index
int storage_set_last_sent(uint32_t index);

// Check if buffer has wrapped (overflowed)
bool storage_is_wrapped(void);

#endif // STORAGE_H

