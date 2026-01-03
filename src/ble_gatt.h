#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"  // ENABLED: storage for sensor data

// UUIDs are defined directly in ble_gatt.c

// Packet types
#define PACKET_TYPE_HEADER 0
#define PACKET_TYPE_DATA    1
#define PACKET_TYPE_END     2

// Control commands
#define CMD_START_TRANSFER  0x01
#define CMD_STOP_TRANSFER   0x02
#define CMD_GET_STATUS      0x03
#define CMD_SET_LAST_SENT   0x04

// Initialize GATT server
int ble_gatt_init(void);

// Start data transfer
int ble_gatt_start_transfer(void);

// Stop data transfer
int ble_gatt_stop_transfer(void);

// Check if transfer is in progress
bool ble_gatt_is_transferring(void);

#endif // BLE_GATT_H

