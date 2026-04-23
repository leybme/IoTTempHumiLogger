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
#include <esp_mac.h>

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
String latestUtcStr  = "";   // always UTC, used for CSV logging
String latestTimeStr = "";   // display timezone (UTC or GMT+7), used for UI

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

// Append one record to today's CSV (always UTC date and UTC timestamp)
void appendRecord(float temp, float humi, const String& utcDt) {
    time_t utc = time(nullptr);
    String path = csvPath(formatDate(utc));  // filename based on UTC date
    File f = SPIFFS.open(path, FILE_APPEND);
    if (!f) {
        // Try creating with header
        f = SPIFFS.open(path, FILE_WRITE);
        if (f) f.println("datetime_utc,temperature_c,humidity_pct");
    }
    if (f) {
        f.printf("%s,%.2f,%.2f\n", utcDt.c_str(), temp, humi);
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
    if (ntpSynced) {
        latestUtcStr  = formatDateTime(time(nullptr));  // always UTC for logging
        latestTimeStr = formatDateTime(nowLocal());      // display tz for UI
    } else {
        latestUtcStr  = "NTP not synced";
        latestTimeStr = "NTP not synced";
    }
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
<title>IoT Logger</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  *{box-sizing:border-box;margin:0;padding:0;}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#f5f5f7;color:#1d1d1f;padding:20px;}
  h1{font-size:1.2rem;font-weight:600;color:#1d1d1f;margin-bottom:20px;letter-spacing:-.3px;}
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:16px;}
  .tile{background:#fff;border-radius:14px;padding:20px 16px;box-shadow:0 1px 4px rgba(0,0,0,.08);}
  .tile .val{font-size:2.2rem;font-weight:700;line-height:1;color:#1d1d1f;}
  .tile .lbl{font-size:.75rem;color:#6e6e73;margin-top:6px;text-transform:uppercase;letter-spacing:.5px;}
  .tile.time .val{font-size:1rem;font-weight:500;margin-top:4px;}
  .card{background:#fff;border-radius:14px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.08);margin-bottom:16px;}
  .card h2{font-size:.85rem;font-weight:600;color:#6e6e73;text-transform:uppercase;letter-spacing:.5px;margin-bottom:16px;}
  .tz-group{display:flex;gap:0;border-radius:8px;overflow:hidden;border:1px solid #d2d2d7;width:fit-content;margin-bottom:16px;}
  .tz-btn{padding:6px 18px;font-size:.85rem;font-weight:500;background:#fff;color:#1d1d1f;border:none;cursor:pointer;transition:background .15s,color .15s;}
  .tz-btn.active{background:#1d1d1f;color:#fff;}
  canvas{max-height:260px;}
  table{width:100%;border-collapse:collapse;}
  th{font-size:.75rem;font-weight:600;color:#6e6e73;text-transform:uppercase;letter-spacing:.5px;padding:0 0 10px;text-align:left;border-bottom:1px solid #e5e5ea;}
  td{padding:10px 0;font-size:.875rem;border-bottom:1px solid #f2f2f7;vertical-align:middle;}
  td:last-child{text-align:right;}
  .btn{display:inline-flex;align-items:center;gap:4px;padding:5px 12px;border-radius:7px;border:1px solid #d2d2d7;background:#fff;font-size:.8rem;font-weight:500;cursor:pointer;color:#1d1d1f;transition:background .15s;}
  .btn:hover{background:#f5f5f7;}
  .btn.danger{border-color:#ffd0d0;color:#c00;background:#fff8f8;}
  .btn.danger:hover{background:#ffd0d0;}
  .cap-bar-wrap{background:#f2f2f7;border-radius:6px;height:6px;margin:10px 0 6px;overflow:hidden;}
  .cap-bar{height:6px;border-radius:6px;background:#1d1d1f;transition:width .4s;}
  .cap-text{font-size:.78rem;color:#6e6e73;display:flex;justify-content:space-between;}
  .badge{display:inline-block;padding:2px 8px;border-radius:20px;font-size:.72rem;font-weight:600;background:#f2f2f7;color:#1d1d1f;margin-left:6px;}
</style>
</head>
<body>
<h1>IoT Temp &amp; Humidity Logger</h1>

<div class="grid">
  <div class="tile">
    <div class="val" id="val-temp">--</div>
    <div class="lbl">Temperature &deg;C</div>
  </div>
  <div class="tile">
    <div class="val" id="val-humi">--</div>
    <div class="lbl">Humidity %</div>
  </div>
  <div class="tile time">
    <div class="lbl">Time</div>
    <div class="val" id="val-time">--</div>
  </div>
</div>

<div class="card">
  <div class="tz-group">
    <button class="tz-btn" id="btn-utc" onclick="setTZ('UTC')">UTC</button>
    <button class="tz-btn active" id="btn-gmt7" onclick="setTZ('GMT7')">GMT+7</button>
  </div>
  <canvas id="chart"></canvas>
</div>

<div class="card">
  <h2>Storage</h2>
  <div class="cap-bar-wrap"><div class="cap-bar" id="cap-bar" style="width:0%"></div></div>
  <div class="cap-text"><span id="cap-used">--</span><span id="cap-days">-- days remaining</span></div>
</div>

<div class="card">
  <h2>Log Files</h2>
  <table>
    <thead><tr><th>Date</th><th>Size</th><th></th></tr></thead>
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
      {label:'Temp (°C)', data:[], borderColor:'#1d1d1f', backgroundColor:'rgba(29,29,31,.06)',
       fill:true, tension:.4, pointRadius:2, borderWidth:2, yAxisID:'y'},
      {label:'Humidity (%)', data:[], borderColor:'#6e6e73', backgroundColor:'rgba(110,110,115,.06)',
       fill:true, tension:.4, pointRadius:2, borderWidth:2, yAxisID:'y2'}
    ]
  },
  options:{
    responsive:true, animation:false,
    plugins:{legend:{labels:{font:{size:12},boxWidth:12}}},
    scales:{
      x:{ticks:{font:{size:11}},grid:{color:'#f2f2f7'}},
      y:{position:'left', title:{display:true,text:'°C',font:{size:11}}, grid:{color:'#f2f2f7'}, ticks:{font:{size:11}}},
      y2:{position:'right', title:{display:true,text:'%',font:{size:11}}, grid:{drawOnChartArea:false}, ticks:{font:{size:11}}}
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
      if(chart.data.labels.length >= MAX_POINTS){
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
      const pct = d.total > 0 ? (d.used/d.total*100).toFixed(1) : 0;
      document.getElementById('cap-bar').style.width = pct+'%';
      document.getElementById('cap-used').textContent = fmt(d.used)+' / '+fmt(d.total)+' ('+pct+'%)';
      document.getElementById('cap-days').textContent = 'Est. '+d.daysRemaining+' days remaining';
      const tbody = document.getElementById('file-list');
      tbody.innerHTML = '';
      d.files.forEach(f=>{
        const tr = document.createElement('tr');
        tr.innerHTML =
          '<td>'+f.date+'</td>'+
          '<td><span class="badge">'+fmt(f.size)+'</span></td>'+
          '<td>'+
            '<button class="btn" onclick="dlFile(\''+f.date+'\')">&#8615; Download</button> '+
            '<button class="btn danger" onclick="rmFile(\''+f.date+'\')">Delete</button>'+
          '</td>';
        tbody.appendChild(tr);
      });
    }).catch(()=>{});
}

function fmt(b){
  if(b<1024) return b+' B';
  if(b<1048576) return (b/1024).toFixed(1)+' KB';
  return (b/1048576).toFixed(2)+' MB';
}

function dlFile(date){window.location.href='/getDate?='+date;}

function rmFile(date){
  if(!confirm('Delete log for '+date+'?')) return;
  fetch('/deleteFile?date='+date).then(()=>loadFiles());
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
    // Add CORS headers to every response
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // ---- Favicon (prevents 500 on browser requests) ----
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204); // No Content
    });

    // ---- Index page ----
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", INDEX_HTML);
    });

    // ---- /getOnce : JSON with latest reading ----
    server.on("/getOnce", HTTP_GET, [](AsyncWebServerRequest* req) {
        readSensor();
        JsonDocument doc;
        doc["temperature"]    = latestTemp;
        doc["humidity"]       = latestHumidity;
        doc["datetime"]       = latestTimeStr;   // display timezone
        doc["datetime_utc"]   = latestUtcStr;    // always UTC
        doc["timezone"]       = useGMT7 ? "GMT+7" : "UTC";
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
            dateStr = formatDate(time(nullptr));  // always UTC date matches log filenames
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
    server.on("/deleteFile", HTTP_GET, [](AsyncWebServerRequest* req) {
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

    // Build AP name from last 3 bytes of MAC: Log_AABBCC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char apName[16];
    snprintf(apName, sizeof(apName), "Log_%02X%02X%02X", mac[3], mac[4], mac[5]);

    // WiFiManager captive portal
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 min timeout
    bool connected = wm.autoConnect(apName);
    if (!connected) {
        Serial.println("WiFi connect failed, restarting...");
        delay(3000);
        ESP.restart();
    }
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // NTP
    setupNTP();

    // Give lwIP time to fully release WiFiManager's port 80 before binding ours
    delay(500);

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
            appendRecord(latestTemp, latestHumidity, latestUtcStr);
            Serial.printf("[LOG] %s  T=%.2f  H=%.2f\n",
                          latestUtcStr.c_str(), latestTemp, latestHumidity);
        }
    }

    delay(100);
}