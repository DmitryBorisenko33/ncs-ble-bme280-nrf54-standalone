#!/usr/bin/env python3
"""
Полный тест концепта: подключение к BME280 ноде и скачивание данных
Проверяет весь workflow: поиск -> подключение -> передача -> сохранение данных
"""
import asyncio
import sys
import struct
import json
import sqlite3
import os
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Optional

try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.characteristic import BleakGATTCharacteristic
except ImportError:
    print("ERROR: bleak not installed")
    print("Install with: pip3 install bleak")
    sys.exit(1)

# UUID definitions
DATA_SERVICE_UUID = "12345678-1234-1234-1234-123456789abc"
DATA_TRANSFER_UUID = "12345678-1234-1234-1234-123456789abd"
CONTROL_UUID = "12345678-1234-1234-1234-123456789abe"
STATUS_UUID = "12345678-1234-1234-1234-123456789abf"

# Control commands
CMD_START_TRANSFER = 0x01
CMD_STOP_TRANSFER = 0x02
CMD_GET_STATUS = 0x03
CMD_SET_LAST_SENT = 0x04

# Packet types
PACKET_TYPE_HEADER = 0
PACKET_TYPE_DATA = 1
PACKET_TYPE_END = 2

# Database
DB_PATH = "sensor_data.db"

class SensorDatabase:
    """Локальная база данных для хранения сенсорных данных, аналогичная приложению"""

    def __init__(self, db_path: str = DB_PATH):
        self.db_path = db_path
        self.init_db()

    def init_db(self):
        """Инициализация базы данных"""
        with sqlite3.connect(self.db_path) as conn:
            conn.execute('PRAGMA journal_mode = WAL')
            conn.execute('''
                CREATE TABLE IF NOT EXISTS records (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    mac TEXT NOT NULL,
                    seq INTEGER NOT NULL,
                    sample_ts_ms INTEGER NOT NULL,
                    rssi INTEGER NOT NULL,
                    temp_x100 INTEGER NOT NULL,
                    press_pa10 INTEGER NOT NULL,
                    hum_x100 INTEGER NOT NULL,
                    battery_mv INTEGER NOT NULL,
                    imported_at_ms INTEGER NOT NULL,
                    UNIQUE(mac, seq)
                )
            ''')
            conn.execute('CREATE INDEX IF NOT EXISTS idx_records_mac_time ON records(mac, sample_ts_ms)')
            conn.execute('CREATE INDEX IF NOT EXISTS idx_records_time ON records(sample_ts_ms)')

            # Таблица для отслеживания синхронизации
            conn.execute('''
                CREATE TABLE IF NOT EXISTS sync_state (
                    mac TEXT PRIMARY KEY,
                    last_synced_seq INTEGER NOT NULL DEFAULT 0,
                    last_sync_time INTEGER NOT NULL,
                    total_synced INTEGER NOT NULL DEFAULT 0
                )
            ''')
            conn.commit()

    def get_sync_state(self, mac: str) -> Dict:
        """Получить состояние синхронизации для устройства"""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.execute(
                'SELECT last_synced_seq, last_sync_time, total_synced FROM sync_state WHERE mac = ?',
                (mac,)
            )
            row = cursor.fetchone()
            if row:
                return {
                    'last_synced_seq': row[0],
                    'last_sync_time': row[1],
                    'total_synced': row[2]
                }
            else:
                # Первая синхронизация
                return {
                    'last_synced_seq': -1,  # Начинаем с -1, чтобы скачать с 0
                    'last_sync_time': 0,
                    'total_synced': 0
                }

    def update_sync_state(self, mac: str, last_synced_seq: int, imported_count: int):
        """Обновить состояние синхронизации"""
        now = int(datetime.now().timestamp() * 1000)
        with sqlite3.connect(self.db_path) as conn:
            conn.execute('''
                INSERT OR REPLACE INTO sync_state (mac, last_synced_seq, last_sync_time, total_synced)
                VALUES (?, ?, ?, COALESCE((SELECT total_synced FROM sync_state WHERE mac = ?), 0) + ?)
            ''', (mac, last_synced_seq, now, mac, imported_count))
            conn.commit()

    def insert_records(self, mac: str, records: List[Dict], rssi: int = -50) -> int:
        """Вставить записи в базу данных"""
        if not records:
            return 0

        now = int(datetime.now().timestamp() * 1000)
        inserted = 0

        with sqlite3.connect(self.db_path) as conn:
            for record in records:
                try:
                    # Конвертируем данные в формат БД (аналогично приложению)
                    seq = record['seq']
                    sample_ts_ms = record['timestamp_ms']

                    # Конвертация значений в формат хранения
                    temp_x100 = int(record['temp_c'] * 100)
                    press_pa10 = int(record['press_kpa'] * 100)  # Паскали * 10
                    hum_x100 = int(record['humidity_pct'] * 100)
                    battery_mv = int(record['battery_v'] * 1000)

                    conn.execute('''
                        INSERT OR IGNORE INTO records
                        (mac, seq, sample_ts_ms, rssi, temp_x100, press_pa10, hum_x100, battery_mv, imported_at_ms)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (mac, seq, sample_ts_ms, rssi, temp_x100, press_pa10, hum_x100, battery_mv, now))

                    if conn.total_changes > 0:
                        inserted += 1

                except Exception as e:
                    print(f"  ⚠ Ошибка вставки записи seq={record.get('seq', '?')}: {e}")

            conn.commit()

        return inserted

    def get_stats(self, mac: str = None) -> Dict:
        """Получить статистику по записям"""
        with sqlite3.connect(self.db_path) as conn:
            if mac:
                cursor = conn.execute('SELECT COUNT(*), MIN(sample_ts_ms), MAX(sample_ts_ms) FROM records WHERE mac = ?', (mac,))
            else:
                cursor = conn.execute('SELECT COUNT(*), MIN(sample_ts_ms), MAX(sample_ts_ms) FROM records')

            row = cursor.fetchone()
            total_records = row[0] if row[0] else 0
            min_time = row[1] if row[1] else 0
            max_time = row[2] if row[2] else 0

            return {
                'total_records': total_records,
                'time_range': {
                    'from': datetime.fromtimestamp(min_time / 1000) if min_time else None,
                    'to': datetime.fromtimestamp(max_time / 1000) if max_time else None
                }
            }

    def list_devices(self) -> List[Dict]:
        """Получить список устройств с их статистикой"""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.execute('''
                SELECT
                    r.mac,
                    COUNT(r.id) as records_count,
                    MIN(r.sample_ts_ms) as first_sample,
                    MAX(r.sample_ts_ms) as last_sample,
                    s.last_synced_seq,
                    s.last_sync_time,
                    s.total_synced
                FROM records r
                LEFT JOIN sync_state s ON r.mac = s.mac
                GROUP BY r.mac
                ORDER BY r.mac
            ''')

            devices = []
            for row in cursor.fetchall():
                mac, count, first, last, last_synced, last_sync, total_synced = row
                devices.append({
                    'mac': mac,
                    'records_count': count,
                    'first_sample': datetime.fromtimestamp(first / 1000) if first else None,
                    'last_sample': datetime.fromtimestamp(last / 1000) if last else None,
                    'last_synced_seq': last_synced,
                    'last_sync_time': datetime.fromtimestamp(last_sync / 1000) if last_sync else None,
                    'total_synced': total_synced or 0
                })

            return devices

    def get_all_records(self, mac: str) -> List[Dict]:
        """Получить все записи для устройства (возвращает в порядке seq ASC)"""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.execute('''
                SELECT seq, sample_ts_ms, rssi, temp_x100, press_pa10, hum_x100, battery_mv
                FROM records
                WHERE mac = ?
                ORDER BY seq ASC
            ''', (mac,))

            rows = []
            for seq, ts, rssi, t100, p10, h100, mv in cursor.fetchall():
                rows.append({
                    'seq': int(seq),
                    'timestamp_ms': int(ts),
                    'rssi': int(rssi),
                    'temp_c': t100 / 100.0,
                    'press_kpa': p10 / 100.0,
                    'humidity_pct': h100 / 100.0,
                    'battery_v': mv / 1000.0,
                })
            return rows

# Глобальный экземпляр базы данных
db = SensorDatabase()

# Storage for received data
received_records = []
transfer_stats = {
    'header_received': False,
    'total_records': 0,
    'data_packets': 0,
    'end_received': False,
    'interval_sec': 0
}

def parse_uint16_be(data, offset):
    """Parse uint16 big-endian"""
    if offset + 2 > len(data):
        return None
    return (data[offset] << 8) | data[offset + 1]

def encode_uint16_be(value):
    """Encode uint16 to big-endian bytes"""
    return bytes([(value >> 8) & 0xFF, value & 0xFF])

def parse_status(data):
    """Parse status characteristic data"""
    if len(data) < 4:
        return None
    return {
        'total': parse_uint16_be(data, 0),
        'last_sent': parse_uint16_be(data, 2),
    }

def parse_sensor_record(data, offset):
    """Parse single sensor record (6 bytes)"""
    if offset + 6 > len(data):
        return None
    
    temp_x10 = struct.unpack('>h', data[offset:offset+2])[0]
    press_kpa = parse_uint16_be(data, offset+2)
    hum_pct = data[offset+4]
    bat_v_x10 = data[offset+5]
    
    return {
        'temp_c': temp_x10 / 10.0,
        'press_kpa': press_kpa,
        'humidity_pct': hum_pct,
        'battery_v': bat_v_x10 / 10.0,
        'temp_raw': temp_x10,
        'press_raw': press_kpa,
        'hum_raw': hum_pct,
        'bat_raw': bat_v_x10
    }

async def scan_and_connect():
    """Scan for device and return client"""
    print(f"⏱️  Старт: {datetime.now().isoformat(timespec='seconds')}")
    print("🔍 Поиск устройства BME280...")
    print(f"   Сервис: {DATA_SERVICE_UUID[:8]}...")

    # Простое сканирование
    devices = await BleakScanner.discover(timeout=10)

    target_address = None
    for device in devices:
        name = device.name or "Unknown"
        if name.startswith("BME-"):
            target_address = device.address
            print(f"✅ Найдено устройство: {name}")
            print(f"   Адрес: {device.address}")
            break

    if not target_address:
        print("❌ Устройство не найдено")
        return None

    print("-" * 70)
    print(f"🔗 Подключение к {target_address}...")

    try:
        print(f"   Попытка подключения...")
        client = BleakClient(target_address)
        await client.connect(timeout=15.0)
        print("✅ Подключено")
        return client
    except Exception as e:
        print(f"✗ Подключение не удалось: {e}")
        return None

async def get_storage_status(client):
    """Get current storage status"""
    try:
        services = client.services
        service_list = list(services)
        status_char = None
        
        for service in service_list:
            if DATA_SERVICE_UUID.lower() in service.uuid.lower():
                for char in service.characteristics:
                    if STATUS_UUID.lower() in char.uuid.lower():
                        status_char = char
                        break
        
        if not status_char or "read" not in status_char.properties:
            return None
        
        data = await client.read_gatt_char(status_char)
        return parse_status(data)
    except Exception as e:
        print(f"  ⚠ Error reading status: {e}")
        return None

async def download_data(client):
    """Download all data from device"""
    device_address = client.address
    print(f"\n🔗 Подключено к {device_address}")
    print("📥 Загрузка данных...")

    # Reset state
    global received_records, transfer_stats
    received_records = []
    transfer_stats = {
        'header_received': False,
        'total_records': 0,
        'data_packets': 0,
        'end_received': False,
        'interval_sec': 0
    }
    
    try:
        services = client.services
        service_list = list(services)
        control_char = None
        data_transfer_char = None
        
        # Find characteristics
        for service in service_list:
            if DATA_SERVICE_UUID.lower() in service.uuid.lower():
                for char in service.characteristics:
                    if CONTROL_UUID.lower() in char.uuid.lower():
                        control_char = char
                    elif DATA_TRANSFER_UUID.lower() in char.uuid.lower():
                        data_transfer_char = char
        
        if not control_char or not data_transfer_char:
            print("✗ Required characteristics not found")
            return False
        
        # Get initial status
        print("\n📊 Анализ данных...")

        initial_status = await get_storage_status(client)
        if not initial_status:
            print("❌ Не удалось прочитать статус устройства")
            return False

        device_total = initial_status['total']
        sync_state = db.get_sync_state(device_address)
        app_last_synced = max(sync_state['last_synced_seq'], -1)  # -1 означает нет данных

        start_index = app_last_synced + 1
        records_to_download = device_total - start_index
        if records_to_download <= 0:
            print("✅ Данные синхронизированы (новых нет)")
            print(f"📍 Устройство: {device_total} записей, приложение: {app_last_synced + 1} записей")
            return True

        print(f"   Устройство: {device_total} записей")
        print(f"   Приложение: синхр. до {app_last_synced}")
        print(f"   Для скачивания: {records_to_download} записей (с {start_index})")

        # Setup notification handler
        transfer_complete = False
        last_packet_time = asyncio.get_event_loop().time()
        
        start_seq = start_index  # запоминаем начальный индекс для seq

        def notification_handler(sender, data):
            nonlocal transfer_complete, last_packet_time
            if len(data) == 0:
                return
            last_packet_time = asyncio.get_event_loop().time()
            print(f"  notif: {data.hex()} (len={len(data)})")
            
            packet_type = data[0]
            
            if packet_type == PACKET_TYPE_HEADER:
                if len(data) >= 3:
                    interval = parse_uint16_be(data, 1)
                    transfer_stats['header_received'] = True
                    transfer_stats['interval_sec'] = interval
                    print(f"  ✓ HEADER received: interval={interval}s")
            
            elif packet_type == PACKET_TYPE_DATA:
                if len(data) >= 5:
                    count = data[3]
                    transfer_stats['data_packets'] += 1
                    transfer_stats['total_records'] += count
                    
                    # Parse records
                    offset = 5
                    for i in range(count):
                        record = parse_sensor_record(data, offset)
                        if record:
                            # Добавляем seq и timestamp
                            record['seq'] = start_seq + i
                            # Генерируем timestamp на основе текущего времени и seq
                            # В реальном приложении timestamp должен приходить от устройства
                            current_time = int(datetime.now().timestamp() * 1000)
                            record['timestamp_ms'] = current_time - (count - 1 - i) * 30000  # 30 сек интервал
                            received_records.append(record)
                            offset += 6
                        else:
                            break
                    
                    if transfer_stats['data_packets'] % 10 == 0:
                        print(f"  Progress: {len(received_records)} records received...")
            
            elif packet_type == PACKET_TYPE_END:
                if len(data) >= 2:
                    total_sent = parse_uint16_be(data, 1)
                    transfer_stats['end_received'] = True
                    transfer_complete = True
                    print(f"  ✓ END received: total_sent={total_sent}")
        
        # Subscribe to notifications
        print("\nSubscribing to data transfer notifications...")
        await client.start_notify(data_transfer_char, notification_handler)
        await asyncio.sleep(0.5)  # Wait for subscription to be ready
        
        # Start transfer
        print(f"🚀 Скачивание {records_to_download} записей...")
        start_cmd = bytes([CMD_START_TRANSFER]) + encode_uint16_be(start_index)
        await client.write_gatt_char(control_char, start_cmd, response=True)
        
        # Wait for transfer to complete (or idle timeout)
        print("Waiting for data transfer...")
        timeout = 20  # hard timeout
        idle_timeout = 3  # idle after last packet
        start_time = asyncio.get_event_loop().time()
        last_packet_time = start_time
        while True:
            await asyncio.sleep(0.1)
            now = asyncio.get_event_loop().time()
            if transfer_complete:
                break
            if (now - last_packet_time) > idle_timeout and transfer_stats['data_packets'] > 0:
                print(f"  ⚠ No packets for {idle_timeout}s, stopping transfer")
                transfer_complete = True
                break
            if (now - start_time) > timeout:
                print(f"  ⚠ Timeout waiting for transfer (>{timeout}s)")
                # Try to stop transfer
                try:
                    await client.write_gatt_char(control_char, bytes([CMD_STOP_TRANSFER]), response=True)
                except:
                    pass
                break
        
        # Stop notifications
        await client.stop_notify(data_transfer_char)
        
        # Get final status and show summary
        print("\n" + "=" * 60)
        print("ИТОГИ ПЕРЕДАЧИ")
        print("=" * 60)

        await asyncio.sleep(0.5)
        final_status = await get_storage_status(client)
        if final_status:
            print(f"📊 Финальное состояние устройства:")
            print(f"   • Всего записей: {final_status['total']}")
            print(f"   • Последняя отправленная: {final_status['last_sent']}")

        # Сохраняем данные в базу
        if received_records:
            inserted = db.insert_records(device_address, received_records, rssi=-50)

            # Обновляем состояние синхронизации
            last_record = max(received_records, key=lambda r: r['seq'])
            new_last_synced = last_record['seq']
            db.update_sync_state(device_address, new_last_synced, len(received_records))

            # Печать всех полученных записей
            print("\nПолученные записи (этот сеанс):")
            for i, r in enumerate(received_records, start=1):
                print(f"  #{i}: T={r['temp_c']:.1f}°C P={r['press_kpa']}kPa H={r['humidity_pct']}% Bat={r['battery_v']:.1f}V")

            print(f"\n💾 Сохранено {inserted} записей")
            print(f"📍 Устройство: {device_total} записей, приложение: {new_last_synced + 1} записей")
            missing = device_total - (new_last_synced + 1)
            if missing > 0:
                print(f"➡️  Нужно ещё получить: {missing} записей")
        else:
            print(f"⚠ Нет записей для сохранения")

        print(f"✅ Передача завершена!")

        # Краткая статистика БД
        stats = db.get_stats()
        print(f"📊 База данных: {stats['total_records']} записей")

        # If no END but есть пакеты/данные — всё равно показываем то, что получили
        if len(received_records) > 0:
            print("\nFirst records:")
            for i, r in enumerate(received_records[:5]):
                print(f"  #{i+1}: T={r['temp_c']:.1f}°C P={r['press_kpa']}kPa H={r['humidity_pct']}% Bat={r['battery_v']:.1f}V")
            if len(received_records) > 5:
                print(f"  ... total {len(received_records)} records")

        if len(received_records) > 0:
            print(f"✅ Данные загружены")
        else:
            print(f"❌ Ошибка загрузки")

        return len(received_records) > 0
    except Exception as e:
        print(f"✗ Error during download: {e}")
        import traceback; traceback.print_exc()
        return False
        
    except Exception as e:
        print(f"✗ Error during download: {e}")
        import traceback
        traceback.print_exc()
        return False

def save_data_to_file(filename=None):
    """Save received data to JSON file"""
    if not received_records:
        print("\n⚠ No data to save")
        return None
    
    if filename is None:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"sensor_data_{timestamp}.json"
    
    output_path = Path("/tmp") / filename
    
    data = {
        'metadata': {
            'download_time': datetime.now().isoformat(),
            'total_records': len(received_records),
            'transfer_stats': transfer_stats
        },
        'records': received_records
    }
    
    with open(output_path, 'w') as f:
        json.dump(data, f, indent=2)
    
    print(f"\n✓ Data saved to: {output_path}")
    print(f"  Total records: {len(received_records)}")
    print(f"  File size: {output_path.stat().st_size} bytes")
    
    return output_path

def print_data_summary():
    """Disabled: подробная сводка не нужна (таблица печатается отдельно)."""
    return

async def main():
    client = None
    try:
        # Step 1: Scan and connect
        client = await scan_and_connect()
        if not client:
            return False
        
        # Step 2: Download data
        success = await download_data(client)
        
        if success:
            # Step 3: Print summary
            print_data_summary()
            
            # Step 4: Save to file
            save_data_to_file()
            
            print("\n" + "=" * 70)
            print("✓ Data download completed successfully!")
            print("=" * 70)
        else:
            print("\n" + "=" * 70)
            print("✗ Data download failed")
            print("=" * 70)
        
        return success
        
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        return False
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        if client and client.is_connected:
            print("\n🔌 Отключение...")
            await client.disconnect()
            print("✅ Отключено")

if __name__ == "__main__":
    try:
        result = asyncio.run(main())
        sys.exit(0 if result else 1)
    except Exception as e:
        print(f"Fatal error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

