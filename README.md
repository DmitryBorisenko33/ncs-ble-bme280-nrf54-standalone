# ncs-ble-bme280-nrf54-standalone

Standalone BME280 sensor node with direct BLE data transfer to mobile phone (nRF54L15 version).

## Features

- **Energy-efficient operation** for battery-powered devices
- **Ring buffer storage** with automatic overwrite (500 KB, ~83,000 records)
- **BLE GATT server** for direct data transfer to phone
- **Connectable advertising** with configurable interval (default 10 seconds)
- **Sensor reading** every 10-30 seconds (configurable)
- **Compact data format** (6 bytes per record)

## Hardware

- nRF54L15 (1.5 MB flash, 256 KB RAM)
- BME280 sensor (I2C)
- Battery powered

## Hardware Specifications

- **Flash**: 1.5 MB (500 KB available for sensor data)
- **RAM**: 256 KB
- **CPU**: ARM Cortex-M33 @ 128 MHz
- **BLE**: Bluetooth 5.4
- **Power**: Optimized for battery operation

## Configuration

Edit `src/config.h` to adjust:
- `SENSOR_READ_INTERVAL_SEC` - sensor reading period (default: 10 seconds)
- `ADV_CONNECTABLE_INTERVAL_MS` - BLE advertising interval (default: 10 seconds)
- `RAM_BUFFER_SIZE` - RAM buffer size before flash write (default: 200 records)
- `FLASH_WRITE_INTERVAL_SEC` - minimum interval between flash writes (default: 5 seconds)

## Building

```bash
west build -b nrf54l15dk/nrf54l15/cpuapp --sysbuild
west flash --runner jlink
```

## Data Format

Each sensor record is 6 bytes:
- `temp_x10` (int16): Temperature in 0.1°C units
- `press_kpa` (uint16): Pressure in kPa
- `hum_pct` (uint8): Humidity in % (0-255)
- `battery_v_x10` (uint8): Battery voltage in 0.1V units

## BLE GATT Service

- **Service UUID**: `12345678-1234-1234-1234-123456789ABC`
- **Data Transfer** (notify): `12345678-1234-1234-1234-123456789ABD`
- **Control** (write): `12345678-1234-1234-1234-123456789ABE`
- **Status** (read/notify): `12345678-1234-1234-1234-123456789ABF`

## Protocol

See plan document for detailed protocol specification.

## Storage

- **Flash partition**: 500 KB (0x7B000 bytes)
- **Max records**: ~83,000 records (6 bytes each)
- **Flash page size**: 4 KB
- **Ring buffer**: Automatic overwrite when full
