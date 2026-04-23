# IoT Temperature & Humidity Logger

ESP32-S3 based data logger using the SHT45 sensor. Logs temperature and humidity to SPIFFS as daily CSV files, serves a live web dashboard with Chart.js, and exposes a simple REST API.

---

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-S3 DevKitM-1 |
| Sensor | Adafruit SHT45 (I²C) |
| SDA | GPIO 9 |
| SCL | GPIO 8 |
| Boot button | GPIO 0 (built-in) |

---

## First Boot — WiFi Setup

1. Power on the device.
2. Connect your phone or laptop to the Wi-Fi AP **`IoTLogger-Setup`**.
3. A captive portal opens automatically (or navigate to `192.168.4.1`).
4. Enter your Wi-Fi SSID and password, then click **Save**.
5. The device restarts and joins your network.

The IP address is printed to the serial monitor (`115200 baud`).

### Reset Wi-Fi Credentials

Hold the **BOOT button (GPIO 0) for 10 seconds**. The device will erase saved credentials and reboot into captive portal mode.

---

## Web Dashboard

Open `http://<device-ip>/` in any browser.

- **Live metrics** — current temperature, humidity, and timestamp.
- **Timezone toggle** — switch between **UTC** and **GMT+7** at any time.
- **Chart.js line chart** — last 60 readings, updated every 1 minute.
- **Log file manager** — lists all daily CSV files with file size, download, and delete buttons.
- **SPIFFS capacity** — shows used / total storage and estimated days remaining.

---

## REST API

### `GET /getOnce`
Returns the latest sensor reading as JSON.

```json
{
  "temperature": 27.35,
  "humidity": 58.12,
  "datetime": "2025-04-23 14:05:00",
  "timezone": "GMT+7"
}
```

---

### `GET /getDate?=<date>`
Downloads the CSV log for the specified day.

| Parameter | Example | Description |
|---|---|---|
| `today` | `/getDate?=today` | Today's log |
| `yyyymmdd` | `/getDate?=20250423` | A specific date |

Returns the raw CSV file as a download, or `404` if no log exists for that date.

---

### `GET /listFiles`
Returns a JSON list of all stored CSV files plus SPIFFS capacity info.

```json
{
  "total": 1572864,
  "used": 32768,
  "daysRemaining": 31,
  "files": [
    { "date": "20250423", "size": 8192, "name": "log_20250423.csv" }
  ]
}
```

---

### `DELETE /deleteFile?date=<yyyymmdd>`
Deletes the log file for the given date.

---

### `GET /capacity`
Returns SPIFFS storage stats and estimated days remaining.

```json
{
  "total": 1572864,
  "used": 32768,
  "free": 1540096,
  "daysRemaining": 31
}
```

---

### `GET /setTZ?tz=UTC` or `?tz=GMT7`
Switches the active timezone. Affects timestamps and the date used for CSV filenames.

---

## CSV Log Format

Files are stored in SPIFFS as `/log_YYYYMMDD.csv`.

```
datetime,temperature_c,humidity_pct
2025-04-23 07:00:00,27.35,58.12
2025-04-23 07:01:00,27.40,57.98
```

One record is written per minute when NTP is synced. Each record is approximately 32 bytes.

---

## Storage Capacity Estimate

| SPIFFS size | Records/day | Est. days |
|---|---|---|
| ~1.5 MB (`min_spiffs`) | 1 440 | ~32 days |

The exact estimate is shown live in the dashboard and via `/capacity`.

---

## Dependencies

| Library | Purpose |
|---|---|
| `Adafruit SHT4x Library` | SHT45 sensor driver |
| `WiFiManager` (tzapu) | Captive portal WiFi setup |
| `ESP Async WebServer` | Non-blocking HTTP server |
| `AsyncTCP` | Async TCP for ESP32 |
| `ArduinoJson` | JSON serialization |

---

## Build & Flash

```bash
# Build
pio run

# Upload
pio run --target upload

# Monitor serial output
pio device monitor
```

Partition table: `min_spiffs.csv` (maximises SPIFFS space on 4 MB flash).
