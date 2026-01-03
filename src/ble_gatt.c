#include "ble_gatt.h"
#include "config.h"
#include "storage.h"  // ENABLED: storage for sensor data
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble_gatt, LOG_LEVEL_DBG);

// UUID definitions
static struct bt_uuid_128 data_service_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABC));
static struct bt_uuid_128 data_transfer_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABD));
static struct bt_uuid_128 control_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABE));
static struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABF));

// Transfer state
static bool transfer_in_progress = false;
static uint32_t transfer_current_index = 0;
static uint32_t transfer_total_count = 0;
static uint32_t transfer_start_seq = 0;  // Starting sequence number for current transfer
static struct bt_conn *current_conn = NULL;

// Characteristic handles
static struct bt_gatt_attr *data_transfer_attr = NULL;
static struct bt_gatt_attr *control_attr = NULL;
static struct bt_gatt_attr *status_attr = NULL;

/* Forward declaration for advertising data (defined in main.c) */
extern struct bt_data ad[3];

static void restart_advertising(void)
{
    LOG_INF("Restarting advertising after disconnect...");
    int err = bt_le_adv_start(
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
                        0x00a0, 0x00a0, NULL),
        ad, ARRAY_SIZE(ad), NULL, 0);
    if (err == -EALREADY) {
        LOG_DBG("Advertising already running");
    } else if (err) {
        LOG_ERR("Failed to restart advertising: %d", err);
    } else {
        LOG_INF("Advertising restarted successfully");
    }
}


// Packet buffer (20 bytes MTU)
static uint8_t packet_buffer[20];

static void encode_u16_be(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v & 0xFF);
}

static void encode_u32_be(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)(v & 0xFF);
}

static int send_header_packet(void)
{
    if (!current_conn || !data_transfer_attr) {
        return -ENOTCONN;
    }

    packet_buffer[0] = PACKET_TYPE_HEADER;
    
    // sensor_interval (2 bytes)
    encode_u16_be(&packet_buffer[1], SENSOR_READ_INTERVAL_SEC);
    
    // total (2 bytes) - max 65535
    uint32_t total = storage_get_count();
    if (total > 65535) total = 65535;
    encode_u16_be(&packet_buffer[3], (uint16_t)total);
    
    // last_sent (2 bytes)
    uint32_t last_sent = storage_get_last_sent();
    if (last_sent > 65535) last_sent = 65535;
    encode_u16_be(&packet_buffer[5], (uint16_t)last_sent);
    
    // reserved (13 bytes) - zero
    memset(&packet_buffer[7], 0, 13);

    struct bt_gatt_notify_params params = {
        .attr = data_transfer_attr,
        .data = packet_buffer,
        .len = sizeof(packet_buffer),
    };

    return bt_gatt_notify_cb(current_conn, &params);
}

static int send_data_packet(uint32_t start_seq, uint8_t count, const sensor_record_t *records)
{
    if (!current_conn || !data_transfer_attr) {
        return -ENOTCONN;
    }

    packet_buffer[0] = PACKET_TYPE_DATA;
    
    // seq (2 bytes)
    if (start_seq > 65535) start_seq = 65535;
    encode_u16_be(&packet_buffer[1], (uint16_t)start_seq);
    
    // count (2 bytes)
    packet_buffer[3] = count;
    packet_buffer[4] = 0;
    
    // data (up to 15 bytes - 2 records max)
    memcpy(&packet_buffer[5], records, count * sizeof(sensor_record_t));
    
    // padding
    memset(&packet_buffer[5 + count * sizeof(sensor_record_t)], 0, 
           15 - count * sizeof(sensor_record_t));

    struct bt_gatt_notify_params params = {
        .attr = data_transfer_attr,
        .data = packet_buffer,
        .len = sizeof(packet_buffer),
    };

    return bt_gatt_notify_cb(current_conn, &params);
}

static int send_end_packet(uint32_t total_sent)
{
    if (!current_conn || !data_transfer_attr) {
        return -ENOTCONN;
    }

    packet_buffer[0] = PACKET_TYPE_END;
    
    // total_sent (2 bytes)
    if (total_sent > 65535) total_sent = 65535;
    encode_u16_be(&packet_buffer[1], (uint16_t)total_sent);
    
    // reserved (17 bytes)
    memset(&packet_buffer[3], 0, 17);

    struct bt_gatt_notify_params params = {
        .attr = data_transfer_attr,
        .data = packet_buffer,
        .len = sizeof(packet_buffer),
    };

    return bt_gatt_notify_cb(current_conn, &params);
}

static void transfer_worker(struct k_work *work)
{
    if (!transfer_in_progress || !current_conn) {
        return;
    }

    // Send header
    if (transfer_current_index == 0) {
        LOG_INF("Sending transfer header, total records: %u", transfer_total_count);
        send_header_packet();
        k_sleep(K_MSEC(50)); // Small delay between packets
    }

    // Send data packets
    sensor_record_t records[2];
    uint32_t records_sent = 0;
    /* start from transfer_start_seq (provided by application) */
    uint32_t start_seq = transfer_start_seq;
    
    while (transfer_current_index < transfer_total_count && records_sent < 100) {
        // Read up to 2 records
        uint8_t count = 0;
        for (uint8_t i = 0; i < 2 && (transfer_current_index + i) < transfer_total_count; i++) {
            if (storage_read(start_seq + transfer_current_index + i, &records[i]) == 0) {
                count++;
            } else {
                /* If read fails, stop transfer and send END with what we have */
                transfer_current_index = transfer_total_count;
                break;
            }
        }

        if (count > 0) {
            send_data_packet(start_seq + transfer_current_index, count, records);
            transfer_current_index += count;
            records_sent += count;
            k_sleep(K_MSEC(50)); // Small delay between packets
        } else {
            break;
        }
    }

    // Send end packet if done
    if (transfer_current_index >= transfer_total_count) {
        LOG_INF("Transfer completed, sent %u records", records_sent);
        send_end_packet(records_sent);
        transfer_in_progress = false;
        transfer_current_index = 0;
    } else {
        LOG_DBG("Transfer progress: %u/%u records", transfer_current_index, transfer_total_count);
        // Schedule next batch
        k_work_submit(work);
    }
}

static K_WORK_DEFINE(transfer_work, transfer_worker);
static K_WORK_DEFINE(advertising_work, restart_advertising);

// Data Transfer Characteristic (notify)
static void data_transfer_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    // Notification enabled/disabled
}

// Control characteristic write handler
static ssize_t control_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *data = (const uint8_t *)buf;
    uint8_t cmd = data[0];

    switch (cmd) {
        case CMD_START_TRANSFER:
            if (len >= 3) {  // CMD + 2 bytes start_index
                uint16_t start_index = sys_get_be16(&data[1]);
                if (!transfer_in_progress) {
                    transfer_in_progress = true;
                    transfer_current_index = 0;
                    transfer_start_seq = start_index;  // Use provided start_index
                    uint32_t total_count = storage_get_count();
                    if (total_count > start_index) {
                        transfer_total_count = total_count - start_index;
                    } else {
                        transfer_total_count = 0;  // No new data
                    }
                    current_conn = bt_conn_ref(conn);
                    LOG_INF("Transfer command received, start_index: %u, total records: %u", start_index, transfer_total_count);
                    k_work_submit(&transfer_work);
                } else {
                    LOG_WRN("Transfer already in progress");
                }
            } else {
                LOG_WRN("Invalid START_TRANSFER command length: %u", len);
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            }
            break;
            
        case CMD_STOP_TRANSFER:
            LOG_INF("Stop transfer command received");
            transfer_in_progress = false;
            if (current_conn) {
                bt_conn_unref(current_conn);
                current_conn = NULL;
            }
            break;
            
        case CMD_SET_LAST_SENT:
            if (len >= 3) {
                uint16_t last_sent = sys_get_be16(&data[1]);
                storage_set_last_sent(last_sent);
            }
            break;
    }

    return len;
}

// Status characteristic read handler
static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    uint8_t status_data[4];
    uint32_t count = storage_get_count();
    uint32_t last_sent = storage_get_last_sent();
    
    if (count > 65535) count = 65535;
    if (last_sent > 65535) last_sent = 65535;
    
    encode_u16_be(&status_data[0], (uint16_t)count);
    encode_u16_be(&status_data[2], (uint16_t)last_sent);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, status_data, sizeof(status_data));
}

BT_GATT_SERVICE_DEFINE(data_service,
    BT_GATT_PRIMARY_SERVICE(&data_service_uuid),
    
    BT_GATT_CHARACTERISTIC(&data_transfer_uuid.uuid,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(data_transfer_ccc_cfg_changed,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    BT_GATT_CHARACTERISTIC(&control_uuid.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, control_write, NULL),
    
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ,
        status_read, NULL, NULL),
);

// Connection callbacks
// NOTE:
// - bt_le_adv_stop() убрано из connected: рекламу не гасим при подключении, чтобы не ломать повторные подключения.
// - Рестарт рекламы только в disconnected (см. restart_advertising), stack resume/таймауты не используются.
static void connected(struct bt_conn *conn, uint8_t err)
{
    LOG_INF("Connected callback called, err=%u", err);
    if (err) {
        LOG_ERR("Connection failed: %u", err);
        return;
    }
    current_conn = bt_conn_ref(conn);

    // Find characteristic attributes for notifications
    data_transfer_attr = bt_gatt_find_by_uuid(NULL, 0, &data_transfer_uuid.uuid);
    control_attr = bt_gatt_find_by_uuid(NULL, 0, &control_uuid.uuid);
    status_attr = bt_gatt_find_by_uuid(NULL, 0, &status_uuid.uuid);

    LOG_INF("BLE client connected, attributes found");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected callback called, reason=%u", reason);
    transfer_in_progress = false;
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    LOG_INF("BLE client disconnected, scheduling advertising restart...");

    // Schedule advertising restart via work queue
    k_work_submit(&advertising_work);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

int ble_gatt_init(void)
{
    int err;

    // Register connection callbacks
    err = bt_conn_cb_register(&conn_callbacks);
    if (err) {
        LOG_ERR("Failed to register connection callbacks: %d", err);
        return err;
    }
    LOG_INF("Connection callbacks registered");

    // Attributes will be found when connection is established
    // using bt_gatt_find_by_uuid if needed

    return 0;
}

int ble_gatt_start_transfer(void)
{
    if (transfer_in_progress) {
        LOG_WRN("Transfer already in progress");
        return -EBUSY;
    }

    transfer_in_progress = true;
    LOG_INF("Starting data transfer from index %u", transfer_start_seq);
    transfer_current_index = 0;
    /* Send records starting from transfer_start_seq */
    uint32_t total = storage_get_count();
    if (total > transfer_start_seq) {
        transfer_total_count = total - transfer_start_seq;
    } else {
        transfer_total_count = 0;
    }
    k_work_submit(&transfer_work);

    return 0;
}

int ble_gatt_stop_transfer(void)
{
    transfer_in_progress = false;
    return 0;
}

bool ble_gatt_is_transferring(void)
{
    return transfer_in_progress;
}

