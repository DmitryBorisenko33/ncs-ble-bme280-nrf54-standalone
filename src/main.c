#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <sys/types.h>
#include "config.h"
#include "storage.h"  // ENABLED: storage for sensor data
#include "ble_gatt.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static uint8_t adv_name[12]; // будет заполнено из BLE адреса, формат BME-XXXXXX

struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL,
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x12, 0x34,
        0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc),
    {
        .type = BT_DATA_NAME_COMPLETE,
        .data_len = 0, // заполнится в рантайме
        .data = adv_name,
    },
};

static void low_power_init(void)
{
    // Low power initialization for nRF54L15
    // Power management handled by Zephyr RTOS
}

// Generate random sensor data instead of reading from real sensor

int main(void)
{
    int err;

    // Early test output via printk (before LOG system initializes)
    // printk("\n\n=== BME280 Node Starting ===\n");
    // printk("UART test: if you see this, UART is working!\n");
    // k_sleep(K_MSEC(100));
    
    // LOG_INF("BME280 Node starting...");

    low_power_init();
    // LOG_DBG("Low power initialized");

    // Initialize storage (non-blocking - continue even if it fails)
    err = storage_init();
    if (err) {
        LOG_ERR("Storage init failed: %d (continuing anyway)", err);
        // Continue without storage - device should still work for BLE
    } else {
        LOG_INF("Storage initialized, records: %u", storage_get_count());
        
        // Write a test record immediately to verify storage works
        sensor_record_t test_record = {
            .temp_x10 = 250,      // 25.0 degC
            .press_kpa = 1013,    // 1013 kPa
            .hum_pct = 50,        // 50%
            .battery_v_x10 = 30   // 3.0V
        };
        storage_write(&test_record);
        LOG_INF("Test record written, total: %u", storage_get_count());
    }

    // Initialize BLE - KEEP THIS (working code from beacon)
    err = bt_enable(NULL);
    if (err) {
        // LOG_ERR("BLE enable failed: %d", err);
        return err;
    }
    // LOG_INF("BLE enabled");

    // Initialize GATT server
    err = ble_gatt_init();
    if (err) {
        LOG_ERR("GATT init failed: %d", err);
        return err;
    }
    LOG_INF("GATT server initialized");

    // Prepare device name from BLE identity address: BME-XXXXXX
    {
        bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
        size_t count = CONFIG_BT_ID_MAX;
        bt_id_get(addrs, &count);
        if (count > 0) {
            const uint8_t *a = addrs[0].a.val;
            int n = snprintk((char *)adv_name, sizeof(adv_name),
                             "BME-%02X%02X%02X", a[5], a[4], a[3]);
            if (n < 0) {
                adv_name[0] = '\0';
                ad[2].data_len = 0;
            } else {
                if (n >= sizeof(adv_name))
                    n = sizeof(adv_name) - 1;
                ad[2].data_len = (uint8_t)n;
            }
            // Set device name for compatibility
            bt_set_name((char *)adv_name);
        } else {
            // Fallback to static short name
            memcpy(adv_name, "BME-FFFF", 8);
            ad[2].data_len = 8;
        }
    }

    // Start BLE advertising - non-connectable legacy, interval ~100 ms (0x00a0 * 0.625 ms)
    // Name is included directly in advertising data (prepared above)
    err = bt_le_adv_start(
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
                        0x00a0, 0x00a0, NULL),
        ad, ARRAY_SIZE(ad),
        NULL, 0);
    if (err) {
        // LOG_ERR("BLE advertising start failed: %d", err);
        return err;
    }
    // LOG_INF("BLE advertising started");

    // LOG_INF("Node initialized successfully");

    // Main loop - generate random sensor data
    static uint32_t counter = 0;
    while (1) {
        sensor_record_t record;
        counter++;
        
        // Generate random sensor data
        // Temperature: 20-30°C (200-300 in x10 units)
        // Use counter and some bit manipulation for pseudo-random values
        uint32_t seed = counter ^ (counter << 13) ^ (counter >> 17);
        record.temp_x10 = 200 + (seed % 100);  // 20.0-29.9°C
        
        // Pressure: 980-1020 kPa (normal atmospheric pressure range)
        seed = seed ^ (seed << 15);
        record.press_kpa = 980 + (seed % 40);  // 980-1019 kPa
        
        // Humidity: 30-80%
        seed = seed ^ (seed << 7);
        record.hum_pct = 30 + (seed % 50);     // 30-79%
        
        // Battery: 3.0-4.2V (30-42 in x10 units)
        seed = seed ^ (seed << 11);
        record.battery_v_x10 = 30 + (seed % 12); // 3.0-4.1V
        
        // Log generated data
        LOG_INF("Random data: T=%d.%d degC P=%d kPa H=%d%% Bat=%d.%dV",
                record.temp_x10 / 10, record.temp_x10 % 10,
                record.press_kpa, record.hum_pct,
                record.battery_v_x10 / 10, record.battery_v_x10 % 10);
        
        // Write to storage
        storage_write(&record);
        
        k_sleep(K_SECONDS(SENSOR_READ_INTERVAL_SEC));
    }
    
    return 0;
}

