#!/bin/bash
# Скрипт для прошивки nRF54L15 DK
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/561dce9adf/opt/zephyr-sdk

cd /Users/dmitry/Desktop/nrf-esp32-gate/ncs-ble-bme280-nrf54-standalone
west build -b nrf54l15dk/nrf54l15/cpuapp --sysbuild
west flash --runner jlink
