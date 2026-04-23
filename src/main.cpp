// IoT Temperature & Humidity Logger
// ESP32-S3 + SHT45/SHT30 + SPIFFS + WiFiManager + AsyncWebServer + NTP
// Features:
//   - WiFi Captive Portal (WiFiManager)
//   - Boot button hold 10s -> reset WiFi credentials & restart captive
//   - NTP time sync, switchable UTC / GMT+7
//   - Log to SPIFFS CSV per day (1 reading/min)
//   - Web chart (Chart.js) with live 1-min update
//   - List / download / delete CSV logs, show sizes & SPIFFS capacity
//   - REST API: /getOnce  /getDate?=today|yyyymmdd
//   - Capacity estimation endpoint
//   - Auto-detects SHT45 or SHT30

#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_SHT4x.h"
#include "Adafruit_SHT31.h"
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
#define LED_PIN       48  // Built-in LED on ESP32-S3

// ---- LED blink intervals ----
#define LED_NORMAL_MS   1000UL  // normal: 1 s
#define LED_FAST_MS     100UL   // WiFi reset pending: 100 ms
#define LED_SLOW_MS     2000UL  // captive portal: 2 s

// ---- NTP ----
#define NTP_SERVER    "pool.ntp.org"
#define TZ_UTC        0
#define TZ_GMT7       (7 * 3600)

// ---- Logging ----
#define LOG_INTERVAL_MS   60000UL   // 1 minute
#define BYTES_PER_RECORD  32        // approximate CSV bytes per record

// ---- Globals ----
Adafruit_SHT4x sht4;
Adafruit_SHT31 sht30;
AsyncWebServer server(80);

enum SensorType { SENSOR_NONE, SENSOR_SHT4X, SENSOR_SHT30 };
SensorType activeSensor = SENSOR_NONE;

bool sensorOK      = false;
bool ntpSynced     = false;
bool useGMT7       = true;   // default GMT+7; toggle via web
unsigned long lastLogMs  = 0;
unsigned long bootBtnMs  = 0;
bool bootBtnHeld   = false;
char deviceId[12]  = "";  // Log_XXXXXX from MAC

// ---- LED state ----
unsigned long lastLedMs   = 0;
bool ledState             = false;
unsigned long ledInterval = LED_NORMAL_MS;

void ledTick() {
    unsigned long now = millis();
    if (now - lastLedMs >= ledInterval) {
        lastLedMs = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
}

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
    if (activeSensor == SENSOR_SHT4X) {
        sensors_event_t hEvent, tEvent;
        sht4.getEvent(&hEvent, &tEvent);
        latestTemp     = tEvent.temperature;
        latestHumidity = hEvent.relative_humidity;
    } else if (activeSensor == SENSOR_SHT30) {
        latestTemp     = sht30.readTemperature();
        latestHumidity = sht30.readHumidity();
        if (isnan(latestTemp) || isnan(latestHumidity)) return false;
    } else {
        return false;
    }
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
  /* Modal */
  .modal-bg{display:none;position:fixed;inset:0;background:rgba(0,0,0,.35);z-index:100;align-items:center;justify-content:center;}
  .modal-bg.open{display:flex;}
  .modal{background:#fff;border-radius:16px;padding:24px;width:min(96vw,720px);box-shadow:0 8px 32px rgba(0,0,0,.18);position:relative;}
  .modal h2{font-size:.95rem;font-weight:600;margin-bottom:16px;color:#1d1d1f;}
  .modal-close{position:absolute;top:14px;right:16px;font-size:1.3rem;background:none;border:none;cursor:pointer;color:#6e6e73;line-height:1;}
  #modal-chart-wrap canvas{max-height:280px;}
</style>
</head>
<body>
<div style="display:flex;align-items:baseline;justify-content:space-between;margin-bottom:20px;">
  <h1 style="margin:0;">IoT Temp &amp; Humidity Logger</h1>
  <span id="device-id" style="font-size:.75rem;color:#6e6e73;font-weight:500;"></span>
</div>

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
  <h2>Storage</h2>
  <div class="cap-bar-wrap"><div class="cap-bar" id="cap-bar" style="width:0%"></div></div>
  <div class="cap-text"><span id="cap-used">--</span><span id="cap-days">-- days remaining</span></div>
</div>

<div class="card">
  <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;">
    <h2 style="margin:0;">Log Files</h2>
    <div class="tz-group" style="margin:0;">
      <button class="tz-btn" id="btn-utc" onclick="setTZ('UTC')">UTC</button>
      <button class="tz-btn active" id="btn-gmt7" onclick="setTZ('GMT7')">GMT+7</button>
    </div>
  </div>
  <table>
    <thead><tr><th>Date</th><th>Size</th><th></th></tr></thead>
    <tbody id="file-list"></tbody>
  </table>
</div>

<!-- Per-day plot modal -->
<div class="modal-bg" id="modal-bg" onclick="if(event.target===this)closeModal()">
  <div class="modal">
    <button class="modal-close" onclick="closeModal()">&#10005;</button>
    <h2 id="modal-title">Plot</h2>
    <div id="modal-chart-wrap"><canvas id="day-chart"></canvas></div>
  </div>
</div>

<script>
function setTZ(tz) {
  fetch('/setTZ?tz='+tz).then(()=>{
    useGMT7 = (tz === 'GMT7');
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
      if(d.device_id) {
        const el = document.getElementById('device-id');
        el.textContent = d.device_id + (d.sensor ? '  ·  ' + d.sensor : '');
      }
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
            '<button class="btn" onclick="openPlot(\''+f.date+'\')">&thinsp;&#9654;&thinsp; Plot</button> '+
            '<button class="btn" onclick="dlFile(\''+f.date+'\')" style="margin-left:4px;">&#8615; Download</button> '+
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

// ---- Per-day plot modal ----
let dayChartInst = null;

function openPlot(date) {
  document.getElementById('modal-title').textContent = 'Plot  –  ' + date;
  document.getElementById('modal-bg').classList.add('open');
  // Fetch CSV as text, parse manually
  fetch('/getDate?='+date)
    .then(r=>r.text())
    .then(csv=>{
      const lines = csv.trim().split('\n');
      const labels=[], temps=[], humis=[];
      const off = useGMT7 ? 7*3600000 : 0;
      for(let i=1;i<lines.length;i++){
        const c = lines[i].split(',');
        if(c.length<3) continue;
        const s = c[0].trim(); // YYYY-MM-DD HH:MM:SS
        const utcMs = Date.UTC(
          parseInt(s.substr(0,4)), parseInt(s.substr(5,2))-1, parseInt(s.substr(8,2)),
          parseInt(s.substr(11,2)), parseInt(s.substr(14,2)), parseInt(s.substr(17,2))
        );
        const d2 = new Date(utcMs + off);
        labels.push(String(d2.getUTCHours()).padStart(2,'0')+':'+String(d2.getUTCMinutes()).padStart(2,'0'));
        temps.push(parseFloat(c[1]));
        humis.push(parseFloat(c[2]));
      }
      if(dayChartInst) dayChartInst.destroy();
      const ctx2 = document.getElementById('day-chart').getContext('2d');
      dayChartInst = new Chart(ctx2, {
        type:'line',
        data:{
          labels,
          datasets:[
            {label:'Temp (°C)', data:temps, borderColor:'#e3342f', backgroundColor:'rgba(227,52,47,.08)',
             fill:true, tension:.4, pointRadius:1, borderWidth:2, yAxisID:'y'},
            {label:'Humidity (%)', data:humis, borderColor:'#3b82f6', backgroundColor:'rgba(59,130,246,.08)',
             fill:true, tension:.4, pointRadius:1, borderWidth:2, yAxisID:'y2'}
          ]
        },
        options:{
          responsive:true, animation:false,
          plugins:{legend:{labels:{font:{size:12},boxWidth:12}}},
          scales:{
            x:{ticks:{font:{size:10}, maxTicksLimit:24}, grid:{color:'#f2f2f7'}},
            y:{position:'left', title:{display:true,text:'°C',font:{size:11}}, grid:{color:'#f2f2f7'}, ticks:{font:{size:11}}},
            y2:{position:'right', title:{display:true,text:'%',font:{size:11}}, grid:{drawOnChartArea:false}, ticks:{font:{size:11}}}
          }
        }
      });
    }).catch(()=>alert('Failed to load data for '+date));
}

let useGMT7 = true; // mirrors server default (GMT+7); updated by setTZ()

function closeModal(){
  document.getElementById('modal-bg').classList.remove('open');
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
    while (!getLocalTime(&ti, 500) && tries++ < 30) {
        Serial.print(".");
        yield();  // feed watchdog during NTP wait
    }
    if (tries < 30) {
        ntpSynced = true;
        Serial.println(" synced.");
    } else {
        Serial.println(" FAILED (will retry later).");
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
        doc["device_id"]      = deviceId;
        doc["sensor"]         = (activeSensor == SENSOR_SHT4X) ? "SHT45" :
                                (activeSensor == SENSOR_SHT30) ? "SHT30" : "none";
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
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Slow blink during captive portal / boot
    ledInterval = LED_SLOW_MS;
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed!");
    } else {
        Serial.printf("SPIFFS: %u / %u bytes used\n", spiffsUsed(), spiffsTotal());
    }

    // Init sensor — try SHT45 first, fall back to SHT30
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setTimeOut(200);  // prevent I2C bus stall from hanging firmware
    if (sht4.begin()) {
        sht4.setPrecision(SHT4X_HIGH_PRECISION);
        sht4.setHeater(SHT4X_NO_HEATER);
        activeSensor = SENSOR_SHT4X;
        sensorOK = true;
        Serial.println("Sensor: SHT45 ready");
    } else if (sht30.begin()) {
        activeSensor = SENSOR_SHT30;
        sensorOK = true;
        Serial.println("Sensor: SHT30 ready");
    } else {
        activeSensor = SENSOR_NONE;
        sensorOK = false;
        Serial.println("No sensor found! (tried SHT45 and SHT30)");
    }

    Wire.clearWriteError();  // clear any I2C error state after probing

    // Build device ID and AP name from last 3 bytes of MAC: Log_AABBCC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(deviceId, sizeof(deviceId), "Log_%02X%02X%02X", mac[3], mac[4], mac[5]);
    char apName[16];
    strncpy(apName, deviceId, sizeof(apName));
    Serial.printf("Device ID: %s\n", deviceId);

    // WiFiManager captive portal (slow blink while waiting)
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 min timeout
    // Use save callback to detect when credentials are saved (portal active = slow blink)
    bool connected = wm.autoConnect(apName);
    if (!connected) {
        Serial.println("WiFi connect failed, restarting...");
        delay(3000);
        ESP.restart();
    }
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // Connected — switch to normal blink
    ledInterval = LED_NORMAL_MS;

    // NTP
    setupNTP();

    // Give lwIP time to fully release WiFiManager's port 80 before binding ours
    delay(500);

    // Web server
    setupWebServer();
}

void loop() {
    ledTick();  // non-blocking LED blink

    // ---- Boot button: hold 10s -> reset WiFi ----
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!bootBtnHeld) {
            bootBtnMs   = millis();
            bootBtnHeld = true;
        }
        // Blink fast while button is held
        ledInterval = LED_FAST_MS;
        if (millis() - bootBtnMs >= 10000UL) {
            resetWiFiAndReboot();
        }
    } else {
        if (bootBtnHeld) ledInterval = LED_NORMAL_MS;  // released before 10s
        bootBtnHeld = false;
    }

    // ---- Sensor retry if not ready (non-blocking: use millis instead of delay) ----
    if (!sensorOK) {
        static unsigned long sensorRetryMs = 0;
        if (millis() - sensorRetryMs >= 2000UL) {
            sensorRetryMs = millis();
            Wire.begin(SDA_PIN, SCL_PIN);
            Wire.setTimeOut(200);
            if (sht4.begin()) {
                sht4.setPrecision(SHT4X_HIGH_PRECISION);
                sht4.setHeater(SHT4X_NO_HEATER);
                activeSensor = SENSOR_SHT4X;
                sensorOK = true;
            } else if (sht30.begin()) {
                activeSensor = SENSOR_SHT30;
                sensorOK = true;
            }
        }
        delay(10);
        return;
    }

    // ---- NTP retry if not synced ----
    if (!ntpSynced) {
        struct tm ti;
        if (getLocalTime(&ti, 100)) {
            ntpSynced = true;
            Serial.println("NTP synced (retry).");
        }
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