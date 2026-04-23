# IoT Temperature & Humidity Logger

ESP32-S3 based data logger using the SHT45 (or SHT30 fallback) sensor. Logs temperature and humidity to SPIFFS as daily CSV files, serves a web dashboard with per-day Chart.js plots, and exposes a simple REST API.

---

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-S3 DevKitM-1 |
| Sensor | Adafruit SHT45 (primary) or SHT30 (auto-fallback), I²C 0x44 |
| SDA | GPIO 9 |
| SCL | GPIO 8 |
| Boot button | GPIO 0 (built-in) |
| LED | GPIO 48 (built-in) |

---

## First Boot — WiFi Setup

1. Power on the device.
2. Connect your phone or laptop to the Wi-Fi AP **`Log_XXXXXX`** (last 3 bytes of MAC address).
3. A captive portal opens automatically (or navigate to `192.168.4.1`).
4. Enter your Wi-Fi SSID and password, then click **Save**.
5. The device restarts and joins your network.

The IP address and device ID are printed to the serial monitor (`115200 baud`).

### Reset Wi-Fi Credentials

Hold the **BOOT button (GPIO 0) for 10 seconds**. The device erases saved credentials and reboots into captive portal mode. The LED blinks fast while the button is held.

### LED Status

| Blink rate | Meaning |
|---|---|
| 1 s | Normal operation |
| 100 ms (fast) | BOOT button held — WiFi reset pending |
| 2 s (slow) | Captive portal active |

---

## Web Dashboard

Open `http://<device-ip>/` in any browser.

- **Live metrics** — current temperature (°C) and humidity (%), updated every 60 s.
- **Device ID & sensor type** — shown in the page header (e.g. `Log_AABBCC · SHT45`).
- **Log file manager** — lists all daily CSV files with file size, and buttons to:
  - **▶ Plot** — opens a Chart.js modal for that day (temperature in red, humidity in blue)
  - **↋ Download** — downloads the raw CSV
  - **Delete** — removes the file from SPIFFS
- **Timezone toggle** (UTC / GMT+7) — in the Log Files header; affects plot labels and the `/setTZ` endpoint.
- **SPIFFS capacity bar** — shows used / total storage and estimated days remaining.

---

## REST API

### `GET /getOnce`
Returns the latest sensor reading as JSON.

```json
{
  "temperature": 27.35,
  "humidity": 58.12,
  "datetime": "2025-04-23 14:05:00",
  "datetime_utc": "2025-04-23 07:05:00",
  "timezone": "GMT+7",
  "device_id": "Log_AABBCC",
  "sensor": "SHT45"
}
```

---

### `GET /getDate?=<date>`
Returns the CSV log for the specified day as a downloadable file.

| Parameter | Example | Description |
|---|---|---|
| `today` | `/getDate?=today` | Today's log (UTC date) |
| `yyyymmdd` | `/getDate?=20250423` | A specific date |

Returns `404` if no log exists for that date. CSV timestamps are always stored in UTC.

---

### `GET /listFiles`
Returns a JSON list of all stored CSV files plus SPIFFS capacity info.

```json
{
  "total": 2097152,
  "used": 32768,
  "daysRemaining": 46,
  "files": [
    { "date": "20250423", "size": 8192, "name": "log_20250423.csv" }
  ]
}
```

---

### `GET /deleteFile?date=<yyyymmdd>`
Deletes the log file for the given date. Returns `{"ok":true}` on success.

---

### `GET /capacity`
Returns SPIFFS storage stats and estimated days remaining.

```json
{
  "total": 2097152,
  "used": 32768,
  "free": 2064384,
  "daysRemaining": 46
}
```

---

### `GET /setTZ?tz=UTC` or `?tz=GMT7`
Switches the display timezone. CSV files always store UTC regardless of this setting.

---

## CSV Log Format

Files are stored in SPIFFS as `/log_YYYYMMDD.csv` (UTC date).

```
datetime_utc,temperature_c,humidity_pct
2025-04-23 07:00:00,27.35,58.12
2025-04-23 07:01:00,27.40,57.98
```

One record is written per minute when NTP is synced. Each record is approximately 32 bytes.

---

## Storage Capacity Estimate

| SPIFFS size | Records/day | Est. days |
|---|---|---|
| ~2 MB (custom `partitions_max_spiffs.csv`) | 1 440 | ~46 days |

The exact estimate is shown live in the dashboard and via `/capacity`.

---

## Partition Table

Custom `partitions_max_spiffs.csv` (no OTA, maximises SPIFFS on 4 MB flash):

| Name | Type | Size |
|---|---|---|
| nvs | data/nvs | 20 KB |
| phy_init | data/phy | 4 KB |
| app0 | app/ota_0 | ~1.98 MB |
| spiffs | data/spiffs | 2 MB |

---

## Dependencies

| Library | Purpose |
|---|---|
| `Adafruit SHT4x Library` | SHT45 sensor driver |
| `Adafruit SHT31 Library` | SHT30 fallback sensor driver |
| `WiFiManager` (tzapu/master) | Captive portal WiFi setup |
| `ESPAsyncWebServer` (mathieucarbou/main) | Non-blocking HTTP server |
| `AsyncTCP` (mathieucarbou/main) | Async TCP for ESP32 |
| `ArduinoJson` v7 | JSON serialization |

---

## Build & Flash

### Using PlatformIO (from source)

```bash
# Build
pio run

# Upload
pio run --target upload

# Monitor serial output
pio device monitor
```

### Upload a pre-built `.bin` using `esptool.py`

Use this method to flash a compiled binary without PlatformIO.

**1. Install esptool**
```bash
pip install esptool
```

**2. Find the COM port**

- Windows: check Device Manager → Ports (COMx)
- Linux/macOS: `/dev/ttyUSB0` or `/dev/tty.usbmodem*`

**3. Flash the binary**

Place all four `.bin` files into a `release/` folder, then run:

```bash
esptool.py --chip esp32s3 --port COM3 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0000  release/bootloader.bin \
  0x8000  release/partitions.bin \
  0xe000  release/boot_app0.bin \
  0x10000 release/firmware.bin
```

Replace `COM3` with your actual port. The four binary files come from the PlatformIO build output:

| File | Source path (after `pio run`) |
|---|---|
| `bootloader.bin` | `.pio/build/esp32-s3-devkitm-1/bootloader.bin` |
| `partitions.bin` | `.pio/build/esp32-s3-devkitm-1/partitions.bin` |
| `boot_app0.bin` | `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin` |
| `firmware.bin` | `.pio/build/esp32-s3-devkitm-1/firmware.bin` |

> **Tip:** If the device is not detected, hold the **BOOT button** while plugging in USB, then release after esptool starts connecting.

