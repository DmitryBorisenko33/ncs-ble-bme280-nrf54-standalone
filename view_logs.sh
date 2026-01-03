#!/bin/bash
# Скрипт для просмотра логов nRF54L15 DK
PORT="/dev/cu.usbmodem0010577085141"
BAUD=115200

echo "=== Логи nRF54L15 DK ==="
echo "Порт: $PORT"
echo "Скорость: $BAUD"
echo ""
echo "Нажмите Ctrl+A затем K для выхода из screen"
echo "================================"
echo ""

screen $PORT $BAUD
