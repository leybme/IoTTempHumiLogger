// IoT Temperature & Humidity Logger
// ESP32-S3 + SHT45 + SPIFFS + WiFiManager + AsyncWebServer + NTP
// Features:
//   - WiFi Captive Portal (WiFiManager)
//   - Boot button hold 10s -> reset WiFi credentials & restart captive
//   - NTP time sync, switchable UTC / GMT+7
//   - Log to SPIFFS CSV per day (1 reading/min)
//   - Web chart (Chart.js) with live 1-min update
//   - List / download / delete CSV logs, show sizes & SPIFFS capacity
//   - REST API: /getOnce  /getDate?=today|yyyymmdd
//   - Capacity estimation endpoint

#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_SHT4x.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// ---- Pin definitions ----
#define SDA_PIN       9
#define SCL_PIN       8
#define BOOT_BTN_PIN  0   // GPIO0 = BOOT button on most ESP32-S3 devkits

// ---- NTP ----
#define NTP_SERVER    "pool.ntp.org"
#define TZ_UTC        0
#define TZ_GMT7       (7 * 3600)

// ---- Logging ----
#define LOG_INTERVAL_MS   60000UL   // 1 minute
#define BYTES_PER_RECORD  32        // approximate CSV bytes per record

// ---- Globals ----
Adafruit_SHT4x sht4;
AsyncWebServer server(80);

bool sensorOK      = false;
bool ntpSynced     = false;
bool useGMT7       = true;   // default GMT+7; toggle via web
unsigned long lastLogMs  = 0;
unsigned long bootBtnMs  = 0;
bool bootBtnHeld   = false;

// Latest reading (updated every loop iteration if sensor OK)
float latestTemp     = 0;
float latestHumidity = 0;
String latestTimeStr = "";

// ---- Helpers ----

// Return current time_t adjusted for selected timezone
time_t nowLocal() {
    time_t utc = time(nullptr);
    if (useGMT7) utc += TZ_GMT7;
    return utc;
}

// Format time_t as "YYYY-MM-DD HH:MM:SS"
String formatDateTime(time_t t) {
    struct tm* tm_info = gmtime(&t);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return String(buf);
}

// Format time_t as "YYYYMMDD"
String formatDate(time_t t) {
    struct tm* tm_info = gmtime(&t);
    char buf[10];
    strftime(buf, sizeof(buf), "%Y%m%d", tm_info);
    return String(buf);
}

// CSV filename for a given date string (yyyymmdd)
String csvPath(const String& dateStr) {
    return "/log_" + dateStr + ".csv";
}

// Append one record to today's CSV
void appendRecord(float temp, float humi, const String& dt) {
    String path = csvPath(formatDate(nowLocal()));
    File f = SPIFFS.open(path, FILE_APPEND);
    if (!f) {
        // Try creating with header
        f = SPIFFS.open(path, FILE_WRITE);
        if (f) f.println("datetime,temperature_c,humidity_pct");
    }
    if (f) {
        f.printf("%s,%.2f,%.2f\n", dt.c_str(), temp, humi);
        f.close();
    }
}

// Read sensor, update globals
bool readSensor() {
    if (!sensorOK) return false;
    sensors_event_t hEvent, tEvent;
    sht4.getEvent(&hEvent, &tEvent);
    latestTemp     = tEvent.temperature;
    latestHumidity = hEvent.relative_humidity;
    latestTimeStr  = ntpSynced ? formatDateTime(nowLocal()) : "NTP not synced";
    return true;
}

// Reset WiFi credentials and reboot into captive portal
void resetWiFiAndReboot() {
    Serial.println("Resetting WiFi credentials...");
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
    ESP.restart();
}

// ---- SPIFFS capacity ----
size_t spiffsTotal() { return SPIFFS.totalBytes(); }
size_t spiffsUsed()  { return SPIFFS.usedBytes(); }

// ---- Estimate how many days of logging remaining ----
int daysRemaining() {
    size_t free = spiffsTotal() - spiffsUsed();
    // records per day = 1440; bytes per day = 1440 * BYTES_PER_RECORD
    size_t bytesPerDay = 1440UL * BYTES_PER_RECORD;
    if (bytesPerDay == 0) return 0;
    return (int)(free / bytesPerDay);
}

// ---- Web HTML (chart + file manager) ----
// Stored in PROGMEM to save RAM
static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IoT Temp/Humi Logger</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:16px;}
  h1{text-align:center;color:#e94560;}
  .card{background:#16213e;border-radius:12px;padding:16px;margin-bottom:16px;}
  .row{display:flex;gap:16px;flex-wrap:wrap;}
  .metric{flex:1;min-width:120px;text-align:center;}
  .metric .val{font-size:2.5em;font-weight:bold;color:#0f3460;}
  .metric .lbl{font-size:.85em;color:#aaa;}
  select,button{padding:8px 14px;border-radius:8px;border:none;cursor:pointer;font-size:1em;}
  button{background:#e94560;color:#fff;margin:4px;}
  button.dl{background:#0f3460;}
  button.rm{background:#7a0000;}
  table{width:100%;border-collapse:collapse;}
  th,td{padding:8px;border-bottom:1px solid #333;text-align:left;font-size:.9em;}
  th{color:#e94560;}
  canvas{max-height:300px;}
  .tz-btn{background:#0f3460;}
  .tz-btn.active{background:#e94560;}
  .cap{font-size:.9em;color:#aaa;margin-top:8px;}
</style>
</head>
<body>
<h1>IoT Temp &amp; Humidity Logger</h1>

<div class="card">
  <div class="row">
    <div class="metric"><div class="val" id="val-temp">--</div><div class="lbl">Temp (&deg;C)</div></div>
    <div class="metric"><div class="val" id="val-humi">--</div><div class="lbl">Humidity (%)</div></div>
    <div class="metric"><div class="val" id="val-time" style="font-size:1em;margin-top:12px;">--</div><div class="lbl">Time</div></div>
  </div>
  <div style="margin-top:12px;text-align:center;">
    <button class="tz-btn" id="btn-utc" onclick="setTZ('UTC')">UTC</button>
    <button class="tz-btn active" id="btn-gmt7" onclick="setTZ('GMT7')">GMT+7</button>
  </div>
</div>

<div class="card">
  <canvas id="chart"></canvas>
</div>

<div class="card">
  <h3 style="margin-top:0;">Log Files</h3>
  <div class="cap" id="capacity">Loading...</div>
  <br>
  <table id="file-table">
    <thead><tr><th>Date</th><th>Size</th><th>Actions</th></tr></thead>
    <tbody id="file-list"></tbody>
  </table>
</div>

<script>
const ctx = document.getElementById('chart').getContext('2d');
const chart = new Chart(ctx, {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {label: 'Temp (°C)', data: [], borderColor: '#e94560', tension: 0.3, yAxisID:'y'},
      {label: 'Humidity (%)', data: [], borderColor: '#0f3460', tension: 0.3, yAxisID:'y2'}
    ]
  },
  options: {
    responsive: true,
    animation: false,
    scales: {
      y:  {position:'left',  title:{display:true,text:'Temp (°C)'}},
      y2: {position:'right', title:{display:true,text:'Humidity (%)'}, grid:{drawOnChartArea:false}}
    }
  }
});

const MAX_POINTS = 60;

function setTZ(tz) {
  fetch('/setTZ?tz='+tz).then(()=>{
    document.getElementById('btn-utc').classList.toggle('active', tz==='UTC');
    document.getElementById('btn-gmt7').classList.toggle('active', tz==='GMT7');
  });
}

function fetchOnce() {
  fetch('/getOnce')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('val-temp').textContent = d.temperature.toFixed(1);
      document.getElementById('val-humi').textContent = d.humidity.toFixed(1);
      document.getElementById('val-time').textContent = d.datetime;
      if (chart.data.labels.length >= MAX_POINTS) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
        chart.data.datasets[1].data.shift();
      }
      chart.data.labels.push(d.datetime.substr(11,5));
      chart.data.datasets[0].data.push(d.temperature);
      chart.data.datasets[1].data.push(d.humidity);
      chart.update();
    }).catch(()=>{});
}

function loadFiles() {
  fetch('/listFiles')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('capacity').innerHTML =
        'SPIFFS: <b>'+fmt(d.used)+'</b> used / <b>'+fmt(d.total)+'</b> total &nbsp;|&nbsp; Est. <b>'+d.daysRemaining+'</b> days remaining';
      const tbody = document.getElementById('file-list');
      tbody.innerHTML = '';
      d.files.forEach(f=>{
        const tr = document.createElement('tr');
        tr.innerHTML = '<td>'+f.date+'</td><td>'+fmt(f.size)+'</td>' +
          '<td><button class="dl" onclick="dlFile(\''+f.date+'\')">Download</button>' +
          '<button class="rm" onclick="rmFile(\''+f.date+'\')">Delete</button></td>';
        tbody.appendChild(tr);
      });
    }).catch(()=>{});
}

function fmt(b){
  if(b<1024) return b+' B';
  if(b<1048576) return (b/1024).toFixed(1)+' KB';
  return (b/1048576).toFixed(2)+' MB';
}

function dlFile(date) {
  window.location.href = '/getDate?='+date;
}

function rmFile(date) {
  if(!confirm('Delete log for '+date+'?')) return;
  fetch('/deleteFile?date='+date, {method:'DELETE'})
    .then(()=>loadFiles());
}

fetchOnce();
loadFiles();
setInterval(fetchOnce, 60000);
setInterval(loadFiles, 30000);
</script>
</body>
</html>
)rawhtml";

// ---- Setup ----

void setupNTP() {
    configTime(0, 0, NTP_SERVER);  // always fetch UTC
    Serial.print("Waiting for NTP...");
    struct tm ti;
    int tries = 0;
    while (!getLocalTime(&ti, 1000) && tries++ < 20) {
        Serial.print(".");
    }
    if (tries < 20) {
        ntpSynced = true;
        Serial.println(" synced.");
    } else {
        Serial.println(" FAILED.");
    }
}

void setupWebServer() {
    // ---- Index page ----
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", INDEX_HTML);
    });

    // ---- /getOnce : JSON with latest reading ----
    server.on("/getOnce", HTTP_GET, [](AsyncWebServerRequest* req) {
        readSensor();
        JsonDocument doc;
        doc["temperature"] = latestTemp;
        doc["humidity"]    = latestHumidity;
        doc["datetime"]    = latestTimeStr;
        doc["timezone"]    = useGMT7 ? "GMT+7" : "UTC";
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ---- /getDate?=today|yyyymmdd : download or JSON ----
    server.on("/getDate", HTTP_GET, [](AsyncWebServerRequest* req) {
        String dateStr;
        if (req->hasParam("")) {
            dateStr = req->getParam("")->value();
        }
        if (dateStr == "today" || dateStr.isEmpty()) {
            dateStr = formatDate(nowLocal());
        }
        String path = csvPath(dateStr);
        if (SPIFFS.exists(path)) {
            req->send(SPIFFS, path, "text/csv", true);
        } else {
            req->send(404, "text/plain", "No log for " + dateStr);
        }
    });

    // ---- /listFiles : JSON list of CSV files ----
    server.on("/listFiles", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["total"] = (int)spiffsTotal();
        doc["used"]  = (int)spiffsUsed();
        doc["daysRemaining"] = daysRemaining();
        JsonArray arr = doc["files"].to<JsonArray>();

        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        while (file) {
            String name = file.name();
            // files named log_YYYYMMDD.csv
            if (name.startsWith("log_") && name.endsWith(".csv")) {
                String date = name.substring(4, 12); // yyyymmdd
                JsonObject o = arr.add<JsonObject>();
                o["date"] = date;
                o["size"] = (int)file.size();
                o["name"] = name;
            }
            file = root.openNextFile();
        }
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ---- /deleteFile?date=yyyymmdd ----
    server.on("/deleteFile", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("date")) { req->send(400, "text/plain", "Missing date"); return; }
        String date = req->getParam("date")->value();
        String path = csvPath(date);
        if (SPIFFS.exists(path)) {
            SPIFFS.remove(path);
            req->send(200, "text/plain", "Deleted");
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });

    // ---- /setTZ?tz=UTC|GMT7 ----
    server.on("/setTZ", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("tz")) {
            String tz = req->getParam("tz")->value();
            useGMT7 = (tz == "GMT7");
        }
        req->send(200, "text/plain", useGMT7 ? "GMT+7" : "UTC");
    });

    // ---- /capacity : estimated days remaining ----
    server.on("/capacity", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["total"]         = (int)spiffsTotal();
        doc["used"]          = (int)spiffsUsed();
        doc["free"]          = (int)(spiffsTotal() - spiffsUsed());
        doc["daysRemaining"] = daysRemaining();
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.begin();
    Serial.println("Web server started.");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Booting IoT Logger...");

    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

    // Mount SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed!");
    } else {
        Serial.printf("SPIFFS: %u / %u bytes used\n", spiffsUsed(), spiffsTotal());
    }

    // Init sensor
    Wire.begin(SDA_PIN, SCL_PIN);
    sensorOK = sht4.begin();
    if (sensorOK) {
        sht4.setPrecision(SHT4X_HIGH_PRECISION);
        sht4.setHeater(SHT4X_NO_HEATER);
        Serial.println("SHT45 ready");
    } else {
        Serial.println("SHT45 not found!");
    }

    // WiFiManager captive portal
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 min timeout
    bool connected = wm.autoConnect("IoTLogger-Setup");
    if (!connected) {
        Serial.println("WiFi connect failed, restarting...");
        delay(3000);
        ESP.restart();
    }
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // NTP
    setupNTP();

    // Web server
    setupWebServer();
}

void loop() {
    // ---- Boot button: hold 10s -> reset WiFi ----
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!bootBtnHeld) {
            bootBtnMs  = millis();
            bootBtnHeld = true;
        } else if (millis() - bootBtnMs >= 10000UL) {
            resetWiFiAndReboot();
        }
    } else {
        bootBtnHeld = false;
    }

    // ---- Sensor retry if not ready ----
    if (!sensorOK) {
        Wire.begin(SDA_PIN, SCL_PIN);
        sensorOK = sht4.begin();
        if (sensorOK) {
            sht4.setPrecision(SHT4X_HIGH_PRECISION);
            sht4.setHeater(SHT4X_NO_HEATER);
        }
        delay(2000);
        return;
    }

    // ---- Log every minute ----
    unsigned long now = millis();
    if (now - lastLogMs >= LOG_INTERVAL_MS) {
        lastLogMs = now;
        if (readSensor() && ntpSynced) {
            appendRecord(latestTemp, latestHumidity, latestTimeStr);
            Serial.printf("[LOG] %s  T=%.2f  H=%.2f\n",
                          latestTimeStr.c_str(), latestTemp, latestHumidity);
        }
    }

    delay(100);
}