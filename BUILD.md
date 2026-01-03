# Build & Flash Instructions

**Target Board:** nRF54L15 Development Kit

## Build

```bash
cd /Users/dmitry/Desktop/nrf-esp32-gate/ncs-ble-bme280-nrf54-standalone
rm -rf build && mkdir build

export ZEPHYR_BASE=/Users/dmitry/ncs/zephyr
export PATH="/Users/dmitry/ncs/.venv/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/561dce9adf/opt/zephyr-sdk

cd build
cmake -GNinja -DBOARD=nrf54l15dk_nrf54l15_cpuapp -DPython3_EXECUTABLE=/Users/dmitry/ncs/.venv/bin/python ..
ninja
```

## Flash

```bash
# Using J-Link (recommended for nRF54L15)
west flash

# Or using openocd with J-Link
openocd -f interface/jlink.cfg -f target/nrf54.cfg -c "program build/zephyr/zephyr.hex verify reset exit"
```

## Alternative: Using West (recommended)

```bash
cd /Users/dmitry/Desktop/nrf-esp32-gate/ncs-ble-bme280-nrf54-standalone
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/561dce9adf/opt/zephyr-sdk
west build -b nrf54l15dk/nrf54l15/cpuapp --sysbuild
west flash --runner jlink
```

## Project Structure

- `src/main.c` - Main application code (sensor reading, BLE advertising)
- `src/storage.c/h` - Flash storage with ring buffer (500 KB for nRF54L15)
- `src/ble_gatt.c/h` - BLE GATT server for data transfer
- `src/config.h` - Configuration constants
- `boards/nrf54l15dk.overlay` - Devicetree overlay for nRF54L15
- `pm.yml` - Partition Manager configuration (OTA support)
- `prj.conf` - Zephyr configuration

## Configuration

Edit `src/config.h` to adjust:
- `SENSOR_READ_INTERVAL_SEC` - Sensor reading period (default: 10 seconds)
- `ADV_CONNECTABLE_INTERVAL_MS` - BLE advertising interval (default: 10 seconds)
- `RAM_BUFFER_SIZE` - RAM buffer size before flash write (default: 200 records)
- `FLASH_WRITE_INTERVAL_SEC` - Minimum interval between flash writes (default: 5 seconds)

## Storage Configuration

- **Flash partition size**: 500 KB (0x7B000 bytes)
- **Max records**: ~83,000 records (6 bytes each)
- **Flash page size**: 4 KB
- **Ring buffer**: Automatic overwrite when full

## Troubleshooting

### Build errors
- Ensure Zephyr environment is properly set up
- Check that all dependencies are installed
- Verify board name: `nrf54l15dk/nrf54l15/cpuapp`
- Make sure you're using NCS v3.0+ (nRF54L15 support)
- Use `--sysbuild` flag for MCUboot support

### Flash errors
- Verify J-Link connection (nRF54L15 DK uses J-Link, not ST-Link)
- Check that J-Link drivers are installed
- Try resetting the board before flashing
- Ensure board is in programming mode

## nRF54L15 Features

- **Flash**: 1.5 MB total (500 KB for sensor data)
- **RAM**: 256 KB
- **CPU**: ARM Cortex-M33 @ 128 MHz
- **BLE**: Bluetooth 5.4
- **OTA**: MCUboot support for over-the-air updates
