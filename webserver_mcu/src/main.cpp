// main.cpp — Webserver MCU (ESP-NOW receiver + NTP + JSON API + POST /api/siren)
// - Serves your honey-themed INDEX_HTML
// - Adds POST /api/siren that parses {"action": "..."} JSON
// - Maps supported actions: "test", "clear_snooze"
// - Returns 400 for unsupported snoozes until siren firmware is extended

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>  // Added for power save control
#include <time.h>
#include <ArduinoJson.h>   // <-- JSON parsing for POST /api/siren

// ================== Wi-Fi (STA) ==================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// ================== NTP (UTC) ==================
const char* NTP_POOL  = "pool.ntp.org";

// ================== Peer MACs (STA MACs) ==================
static const uint8_t MAC_SIREN[6] = {0x00,0x00,0x00,0x00,0x00,0x00}; // Replace with Siren STA MAC
static const uint8_t MAC_SENSORS[3][6] = {
  {0x00,0x00,0x00,0x00,0x00,0x00}, // Replace with Sensor 1 STA MAC
  {0x00,0x00,0x00,0x00,0x00,0x00}, // Replace with Sensor 2 STA MAC  
  {0x00,0x00,0x00,0x00,0x00,0x00}  // Replace with Sensor 3 STA MAC
};


// ================== Packets ==================
#pragma pack(push,1)
struct SensorPacket {
  uint8_t  ver;          // 1
  uint8_t  tank_id;      // 0/1/2
  uint16_t distance_mm;  // median over 5 s (0 if invalid)
  uint16_t battery_mV;   // 0 if unused
  uint8_t  flags;        // bit0: valid_median, bit1: at_risk_le_6cm
  uint8_t  crc8;         // CRC-8 over [ver..flags]
};

struct CommandPacket {   // Webserver -> Siren
  uint8_t  ver;       // 1
  uint8_t  type;      // 0xC1
  uint8_t  cmd;       // 1=FORCE_ON, 2=FORCE_OFF, 3=SNOOZE_5MIN, 4=CLEAR_SNOOZE, 5=SNOOZE_CUSTOM_MS
  uint8_t  tank_id;   // 0/1/2 or 255=ALL
  uint16_t ms;        // used for FORCE_ON and SNOOZE_CUSTOM_MS
  uint8_t  crc8;      // CRC-8 over [ver..ms]
};
#pragma pack(pop)

// CRC-8-ATM (poly 0x07, init 0x00)
static uint8_t crc8(const uint8_t* d, size_t n) {
  uint8_t c = 0;
  for (size_t i=0;i<n;i++){ c^=d[i]; for(int b=0;b<8;b++) c = (c&0x80)? (uint8_t)((c<<1)^0x07):(uint8_t)(c<<1); }
  return c;
}

// ================== State (latest per tank) ==================
static const int MAX_TANKS = 3;
static float    lastDistanceCm[MAX_TANKS] = {NAN,NAN,NAN};
static uint16_t lastBattery_mV[MAX_TANKS] = {0,0,0};
static uint32_t lastRxMillis[MAX_TANKS]   = {0,0,0};  // monotonic for "ago"
static time_t   lastRxEpoch[MAX_TANKS]    = {0,0,0};  // UTC wall time (once NTP syncs)

// ================== HTTP server ==================
WebServer server(80);

// ================== Storage note (please read) ==================
// This project only keeps static assets (e.g., INDEX_HTML[]) in flash/PROGMEM.
// Do NOT log sensor history or frequent telemetry to internal flash
// (SPIFFS/LittleFS/NVS). ESP32 QSPI flash has limited erase cycles (~10–100k
// per sector), so appending data every wake can wear it out quickly.
//
// For time-series or large data, use an external medium instead:
//   • microSD (SPI or SDMMC) — libraries: <SD.h>, <SD_MMC.h>, SdFat
//   • I²C/SPI FRAM (e.g., Fujitsu MB85RC…/MB85RS…) — virtually unlimited writes
//   • Stream to a server and store in a proper database
//
// If you must persist small settings, keep them in NVS or LittleFS and write rarely
// (batch/ratelimit to minutes or hours; avoid per-reading writes).
//
// TL;DR: flash = code + static files; SD/FRAM/server = logs/history.


// ================== Your themed INDEX_HTML ==================
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Honey Tanks</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
            background: linear-gradient(135deg, #fff8e1 0%, #ffecb3 100%);
            color: #3e2723; line-height: 1.4; min-height: 100vh;
        }
        .container { max-width: 800px; margin: 0 auto; padding: 20px; }
        .header { text-align: center; margin-bottom: 30px;
            background: linear-gradient(135deg, #ffb300 0%, #ff8f00 100%);
            color: white; padding: 20px; border-radius: 10px;
            box-shadow: 0 4px 15px rgba(255, 143, 0, 0.3);
        }
        .header h1 { font-size: 2.5rem; color: white; margin-bottom: 10px;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.2);}
        .ntp-status { font-size: 0.9rem; padding: 5px 15px; border-radius: 20px; display: inline-block; }
        .ntp-synced { background: #c8e6c9; color: #2e7d32; }
        .ntp-not-synced { background: #ffcccb; color: #c62828; }
        .tanks { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
                 gap: 20px; margin-bottom: 30px; }
        .tank-card { background: linear-gradient(145deg, #fff3e0 0%, #ffe0b2 100%);
            border: 2px solid #ffb74d; border-radius: 15px; padding: 25px;
            box-shadow: 0 4px 15px rgba(255, 183, 77, 0.2); text-align: center; transition: transform 0.2s; }
        .tank-card:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(255, 183, 77, 0.3); }
        .tank-card.offline { background: linear-gradient(145deg, #efebe9 0%, #d7ccc8 100%);
            border-color: #a1887f; opacity: 0.8; }
        .tank-title { font-size: 1.3rem; font-weight: bold; margin-bottom: 15px; color: #bf360c; }
        .distance { font-size: 3rem; font-weight: bold; margin-bottom: 15px; color: #e65100; }
        .distance.offline { color: #8d6e63; }
        .status-chip { display: inline-block; padding: 8px 16px; border-radius: 25px; font-weight: bold; font-size: 0.9rem; margin-bottom: 15px; text-transform: uppercase; }
        .status-ok { background: #c8e6c9; color: #1b5e20; }
        .status-at-risk { background: #ffcdd2; color: #b71c1c; }
        .status-offline { background: #bcaaa4; color: #3e2723; }
        .last-update { font-size: 0.95rem; color: #6c757d; margin-bottom: 10px; }
        .battery { font-size: 0.85rem; color: #6c757d; }
        .fill-bar-container { width: 100%; height: 120px; background: #f3e5ab; border-radius: 8px; margin: 15px 0; position: relative; border: 2px solid #d4af37; }
        .fill-bar { width: 100%; background: linear-gradient(to top, #d4af37, #ffd700, #ffb300);
            border-radius: 6px; transition: height 0.5s ease; position: absolute; bottom: 0; box-shadow: inset 0 2px 4px rgba(0,0,0,0.1); }
        .fill-percentage { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%);
            font-weight: bold; color: #3e2723; font-size: 0.9rem; text-shadow: 1px 1px 2px rgba(255,255,255,0.8); }
        .waiting-connection { color: #6c757d; font-style: italic; }
        .control-panel { background: linear-gradient(135deg, #3e2723 0%, #5d4037 100%); color: #fff8e1; border: 2px solid #8d6e63; }
        .control-panel .tank-title { color: #fff8e1; margin-bottom: 20px; }
        .control-buttons { display: flex; flex-direction: column; gap: 10px; }
        .control-btn { background: rgba(255,255,255,0.2); border: 1px solid rgba(255,255,255,0.3);
            color: white; padding: 12px 16px; border-radius: 8px; cursor: pointer; font-size: 0.9rem; font-weight: 500; transition: all 0.2s; }
        .control-btn:hover { background: rgba(255,255,255,0.3); transform: translateY(-1px); }
        .control-btn:active { transform: translateY(0); }
        .control-btn.test { background: rgba(255,193,7,0.4); border-color: rgba(255,193,7,0.6); }
        .control-btn.clear { background: rgba(139,195,74,0.4); border-color: rgba(139,195,74,0.6); }
        .footer { text-align: center; font-size: 0.8rem; color: #5d4037;
            background: linear-gradient(145deg, #fff3e0 0%, #ffe0b2 100%); border: 2px solid #ffb74d; padding: 15px; border-radius: 10px; box-shadow: 0 2px 10px rgba(255,183,77,0.2); }
        .footer div { margin: 2px 0; }
        .loading { text-align: center; padding: 50px; color: #8d6e63; }
        @media (max-width: 600px) {
            .container { padding: 15px; }
            .header h1 { font-size: 2rem; }
            .distance { font-size: 2.5rem; }
            .tank-card { padding: 20px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🍯 Warcola Honey Farms</h1>
            <div class="ntp-status" id="ntpStatus">Syncing...</div>
        </div>
        <div class="tanks" id="tanksContainer">
            <div class="loading">Waiting for sensor data...</div>
        </div>
        <div class="footer" id="footer">
            <div>Loading...</div>
        </div>
    </div>
    <script>
        let lastUpdateTime = Date.now();
        let hasReceivedData = false;
        const TANK_HEIGHTS = { 0: 90, 1: 85, 2: 95 };

        function formatTime(dateStr){ if(!dateStr) return 'Never'; const d=new Date(dateStr); return d.toLocaleTimeString('en-US',{hour12:false}); }
        function formatTimeSince(sec){ if(sec==null) return 'Unknown'; if(sec<60) return `${sec}s ago`; const m=Math.floor(sec/60); if(m<60) return `${m}m ago`; const h=Math.floor(m/60); return `${h}h ${m%60}m ago`; }
        function calculateHoneyLevel(id, dist){ if(dist==null) return null; const h=TANK_HEIGHTS[id]||90; const lvl=h - dist; return Math.max(0, lvl); }
        function calculateFillPercentage(id, dist){ const lvl=calculateHoneyLevel(id, dist); if(lvl==null) return 0; const h=TANK_HEIGHTS[id]||90; return Math.min(100, Math.max(0, (lvl/h)*100)); }

        async function sirenControl(action){
          try{
            const r = await fetch('/api/siren', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({action}) });
            const j = await r.json().catch(()=>null);
            if(!r.ok){ console.error('Siren action failed', r.status, j||''); alert(j && j.error ? j.error : `Siren action failed (${r.status})`); }
          }catch(e){ console.error('Siren action error', e); alert('Siren action error'); }
        }

        function updateTankDisplay(data){
          const c = document.getElementById('tanksContainer');
          const ntp = document.getElementById('ntpStatus');
          hasReceivedData = true;
          ntp.textContent = data.server_time_iso ? 'Synced' : 'Not Synced';
          ntp.className = `ntp-status ${data.server_time_iso ? 'ntp-synced' : 'ntp-not-synced'}`;

          let html = `
            <div class="tank-card control-panel">
              <div class="tank-title">🚨 Siren Control</div>
              <div class="control-buttons">
                <button class="control-btn test"  onclick="sirenControl('test')">Test Siren</button>
                <button class="control-btn"       onclick="sirenControl('snooze_10m')">Snooze 10 Minutes</button>
                <button class="control-btn"       onclick="sirenControl('snooze_20m')">Snooze 20 Minutes</button>
                <button class="control-btn"       onclick="sirenControl('snooze_1h')">Snooze 1 Hour</button>
                <button class="control-btn clear" onclick="sirenControl('clear_snooze')">Clear Snooze</button>
              </div>
            </div>
          `;

          for(let i=0;i<3;i++){
            const t = (data.tanks||[]).find(x=>x.tank_id===i) || {tank_id:i,distance_cm:null,at_risk:false,last_update_iso:null,last_seen_secs_ago:null,battery_mV:0,offline:true};
            const offline = t.offline || (t.last_seen_secs_ago>300);
            const hasData = t.distance_cm!=null;
            let statusClass='status-offline', statusText='OFFLINE';
            if(!offline && hasData){ if(t.at_risk){statusClass='status-at-risk'; statusText='AT RISK';} else {statusClass='status-ok'; statusText='OK';} }
            const distText = hasData ? `${t.distance_cm.toFixed(1)} cm` : '--';
            const honeyLvl = calculateHoneyLevel(i, t.distance_cm);
            const fillPct  = calculateFillPercentage(i, t.distance_cm);
            const honeyText = honeyLvl!=null ? `${honeyLvl.toFixed(1)} cm` : '--';
            const bat = t.battery_mV>0 ? `<div class="battery">🔋 ${(t.battery_mV/1000).toFixed(2)}V</div>` : '';
            html += `
              <div class="tank-card ${offline?'offline':''}">
                <div class="tank-title">Tank ${i+1}</div>
                <div class="distance ${offline?'offline':''}">
                  Distance: ${distText}<br><small>Honey: ${honeyText}</small>
                </div>
                <div class="fill-bar-container">
                  <div class="fill-bar" style="height:${fillPct}%"></div>
                  <div class="fill-percentage">${hasData? (fillPct|0)+'%':'--'}</div>
                </div>
                <div class="status-chip ${statusClass}">${statusText}</div>
                <div class="last-update">${formatTimeSince(t.last_seen_secs_ago)} (${formatTime(t.last_update_iso)})</div>
                ${bat}
              </div>`;
          }
          c.innerHTML = html;
          lastUpdateTime = Date.now();
        }

        function showWaitingConnection(){
          const c = document.getElementById('tanksContainer');
          const ntp = document.getElementById('ntpStatus');
          ntp.textContent='Waiting...'; ntp.className='ntp-status ntp-not-synced';
          let html = `
            <div class="tank-card control-panel">
              <div class="tank-title">🚨 Siren Control</div>
              <div class="control-buttons">
                <button class="control-btn test"  onclick="sirenControl('test')">Test Siren</button>
                <button class="control-btn"       onclick="sirenControl('snooze_10m')">Snooze 10 Minutes</button>
                <button class="control-btn"       onclick="sirenControl('snooze_20m')">Snooze 20 Minutes</button>
                <button class="control-btn"       onclick="sirenControl('snooze_1h')">Snooze 1 Hour</button>
                <button class="control-btn clear" onclick="sirenControl('clear_snooze')">Clear Snooze</button>
              </div>
            </div>`;
          for(let i=0;i<3;i++){
            html += `
              <div class="tank-card">
                <div class="tank-title">Tank ${i+1}</div>
                <div class="distance waiting-connection">Distance: --<br><small>Honey: --</small></div>
                <div class="fill-bar-container"><div class="fill-bar" style="height:0%"></div><div class="fill-percentage">--</div></div>
                <div class="status-chip status-offline">WAITING CONNECTION</div>
                <div class="last-update waiting-connection">No data received yet</div>
              </div>`;
          }
          c.innerHTML = html;
        }

        function updateFooter(){
          const f = document.getElementById('footer');
          f.innerHTML = `
            <div>MAC: CC:DB:A7:92:C2:B8</div>
            <div>Last refresh: ${new Date(lastUpdateTime).toLocaleTimeString()}</div>
            <div>Version: 1.0</div>`;
        }

        async function fetchData(){
          try{
            const r = await fetch('/api/status');
            if(!r.ok) throw new Error('HTTP '+r.status);
            const j = await r.json();
            updateTankDisplay(j);
            updateFooter();
          }catch(err){
            console.error('Failed to fetch data:', err);
            if(!hasReceivedData){
              showWaitingConnection();
            }else{
              document.getElementById('tanksContainer').innerHTML =
                `<div class="loading" style="color:#dc3545;">Connection Error<br><small>Retrying in 10s...</small></div>`;
            }
          }
        }

        showWaitingConnection(); fetchData(); updateFooter();
        setInterval(fetchData, 10000); setInterval(updateFooter, 1000);
    </script>
</body>
</html>)HTML";

// ================== Utilities ==================
static String iso8601_utc(time_t t) {
  if (t <= 0) return String();
  struct tm tm{};
  gmtime_r(&t, &tm);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

static bool ntpSynced() { return time(nullptr) > 1609459200; } // > 2021-01-01

static bool macEquals(const uint8_t *a, const uint8_t *b) { return memcmp(a,b,6)==0; }
static int tankIdFromMac(const uint8_t *mac) {
  for (int i=0;i<MAX_TANKS;i++) if (macEquals(mac, MAC_SENSORS[i])) return i;
  return -1;
}

// ================== ESP-NOW receive ==================
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) { 
  Serial.printf("ESP-NOW RX: %02X:%02X:%02X:%02X:%02X:%02X len=%d\n", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], len);
  
  if (len != (int)sizeof(SensorPacket)) {
    Serial.printf("Wrong packet size, expected %d got %d\n", sizeof(SensorPacket), len);
    return;
  }

  SensorPacket p;
  memcpy(&p, data, sizeof(p));

  // CRC & version
  auto calc = crc8((const uint8_t*)&p, sizeof(p)-1);
  if (p.crc8 != calc) {
    Serial.printf("CRC mismatch: expected %02X got %02X\n", calc, p.crc8);
    return;
  }
  if (p.ver != 1) {
    Serial.printf("Wrong version: %d\n", p.ver);
    return;
  }
  if (p.tank_id >= MAX_TANKS) {
    Serial.printf("Invalid tank_id: %d\n", p.tank_id);
    return;
  }

  // Optional: verify claimed tank_id matches known MAC mapping
  int expected = tankIdFromMac(mac);
  if (expected >= 0 && (uint8_t)expected != p.tank_id) {
    Serial.printf("Tank ID mismatch: MAC suggests %d but packet claims %d\n", expected, p.tank_id);
    return;
  }

  const bool valid = (p.flags & 0x01) && p.distance_mm>0;
  const float d_cm = valid ? (p.distance_mm / 10.0f) : NAN;

  Serial.printf("Tank %d: distance=%.1fcm battery=%dmV flags=0x%02X valid=%s\n", 
    p.tank_id, d_cm, p.battery_mV, p.flags, valid ? "YES" : "NO");

  lastDistanceCm[p.tank_id] = d_cm;
  lastBattery_mV[p.tank_id] = p.battery_mV;
  lastRxMillis[p.tank_id]   = millis();
  lastRxEpoch[p.tank_id]    = ntpSynced()? time(nullptr) : 0;
}

// ================== ESP-NOW command sending ==================
static bool addPeer(const uint8_t mac[6]) {
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;    // current channel
  peer.encrypt = false;
  esp_err_t result = esp_now_add_peer(&peer);
  Serial.printf("Added peer %02X:%02X:%02X:%02X:%02X:%02X: %s\n", 
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 
    (result == ESP_OK) ? "OK" : "FAILED");
  return (result == ESP_OK);
}

static bool sendCommand(uint8_t cmd, uint8_t tank_id, uint16_t ms=0) {
  CommandPacket c{};
  c.ver  = 1;
  c.type = 0xC1;
  c.cmd  = cmd;
  c.tank_id = tank_id;   // 0/1/2 or 255 for ALL
  c.ms   = ms;
  c.crc8 = crc8((uint8_t*)&c, sizeof(c)-1);
  esp_err_t result = esp_now_send(MAC_SIREN, (uint8_t*)&c, sizeof(c));
  Serial.printf("Command sent to siren: cmd=%d tank=%d ms=%d result=%s\n", 
    cmd, tank_id, ms, (result == ESP_OK) ? "OK" : "FAILED");
  return result == ESP_OK;
}

// ================== HTTP handlers ==================
static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  const uint32_t nowMs = millis();
  time_t nowEpoch = time(nullptr);
  String nowIso   = ntpSynced()? iso8601_utc(nowEpoch) : String();

  String json = "{\"server_time_iso\":\"";
  json += nowIso;
  json += "\",\"ntp_synced\":";
  json += (ntpSynced()? "true":"false");
  json += ",\"wifi_channel\":";
  json += String(WiFi.channel());
  json += ",\"tanks\":[";
  for (int i=0;i<MAX_TANKS;i++) {
    if (i) json += ",";
    bool have = !isnan(lastDistanceCm[i]);
    bool offline = (!lastRxMillis[i]) || (nowMs - lastRxMillis[i] > 5UL*60UL*1000UL); // >5 min
    bool at_risk = have && (lastDistanceCm[i] <= 6.0f);

    json += "{\"tank_id\":";
    json += String(i);
    json += ",\"distance_cm\":";
    if (have) { json += String(lastDistanceCm[i], 1); } else { json += "null"; }
    json += ",\"at_risk\":";
    json += (at_risk? "true":"false");
    json += ",\"last_update_iso\":";
    if (lastRxEpoch[i] > 0) { json += "\"" + iso8601_utc(lastRxEpoch[i]) + "\""; } else { json += "null"; }
    json += ",\"last_seen_secs_ago\":";
    if (lastRxMillis[i] == 0) { json += "null"; }
    else { json += String((nowMs - lastRxMillis[i]) / 1000UL); }
    json += ",\"battery_mV\":";
    json += String((unsigned)lastBattery_mV[i]);
    json += ",\"offline\":";
    json += (offline? "true":"false");
    json += "}";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

// POST /api/siren  with JSON: {"action":"test" | "snooze_10m" | "snooze_20m" | "snooze_1h" | "clear_snooze"}
// Optional: support {"tank": 0|1|2|"all"} later; defaults to ALL tanks (255)
static void handleSirenPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  const String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", String("{\"error\":\"bad json: ") + err.c_str() + "\"}");
    return;
  }

  const char* action = doc["action"] | "";
  if (!action || !*action) {
    server.send(400, "application/json", "{\"error\":\"missing action\"}");
    return;
  }

  // Default to ALL tanks; you can add per-tank control by reading doc["tank"]
  uint8_t tank_id = 255;

  bool ok = false;

  if (strcmp(action, "test") == 0) {
    ok = sendCommand(/*FORCE_ON*/1, tank_id, /*ms*/5000);
    server.send(200, "application/json", String("{\"ok\":")+(ok?"true}":"false}"));
    return;
  }

  if (strcmp(action, "clear_snooze") == 0) {
    ok = sendCommand(/*CLEAR_SNOOZE*/4, tank_id, 0);
    server.send(200, "application/json", String("{\"ok\":")+(ok?"true}":"false}"));
    return;
  }

  // New: map your UI snoozes to SNOOZE_CUSTOM_MS (cmd=5)
  if (strcmp(action, "snooze_10m") == 0) {
    ok = sendCommand(/*SNOOZE_CUSTOM_MS*/5, tank_id, 10*60*1000);
    server.send(200, "application/json", String("{\"ok\":")+(ok?"true}":"false}"));
    return;
  }
  if (strcmp(action, "snooze_20m") == 0) {
    ok = sendCommand(/*SNOOZE_CUSTOM_MS*/5, tank_id, 20*60*1000);
    server.send(200, "application/json", String("{\"ok\":")+(ok?"true}":"false}"));
    return;
  }
  if (strcmp(action, "snooze_1h") == 0) {
    ok = sendCommand(/*SNOOZE_CUSTOM_MS*/5, tank_id, 60*60*1000);
    server.send(200, "application/json", String("{\"ok\":")+(ok?"true}":"false}"));
    return;
  }

  server.send(400, "application/json", "{\"error\":\"unknown action\"}");
}

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWebserver MCU booting…");

  // 1) Wi-Fi STA
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  for (int i=0; i<60 && WiFi.status()!=WL_CONNECTED; ++i) { delay(500); Serial.print("."); }
  Serial.println();
  
  if (WiFi.status()==WL_CONNECTED) {
    // CRITICAL FIX: Disable power save immediately after connection
    esp_err_t ps_result = esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.printf("WiFi power save disabled: %s\n", (ps_result == ESP_OK) ? "OK" : "FAILED");
    
    Serial.printf("Connected. IP=%s  RSSI=%ddBm  CH=%d  MAC=%s\n",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI(), WiFi.channel(),
      WiFi.macAddress().c_str());
  } else {
    Serial.println("WiFi not connected (continuing; ESPNOW receive still works on current channel).");
  }

  // 2) NTP UTC
  configTime(0, 0, NTP_POOL);
  Serial.println("NTP requested (UTC).");

  // 3) ESP-NOW - Initialize AFTER WiFi power save is disabled
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
  } else {
    // Register callback FIRST
    esp_err_t cb_result = esp_now_register_recv_cb(onDataRecv);
    Serial.printf("ESP-NOW receive callback registered: %s\n", (cb_result == ESP_OK) ? "OK" : "FAILED");
    
    // Add all sensor peers for better RX reliability
    for (int i = 0; i < MAX_TANKS; i++) {
      esp_now_peer_info_t sensor_peer{};
      memcpy(sensor_peer.peer_addr, MAC_SENSORS[i], 6);
      sensor_peer.channel = 0;  // Use current WiFi channel
      sensor_peer.encrypt = false;
      esp_err_t add_result = esp_now_add_peer(&sensor_peer);
      Serial.printf("Added sensor %d peer: %s\n", i, (add_result == ESP_OK) ? "OK" : "FAILED");
    }
    
    // Add siren peer for sending commands
    addPeer(MAC_SIREN);
    Serial.println("ESP-NOW ready.");
  }

  // Small delay to ensure ESP-NOW is fully initialized
  delay(50);

  // 4) HTTP routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/siren", HTTP_POST, handleSirenPost);

  // Legacy optional GET endpoints
  server.on("/api/force_on",    HTTP_GET, [](){ bool ok=sendCommand(1,255,5000); server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}")); });
  server.on("/api/force_off",   HTTP_GET, [](){ bool ok=sendCommand(2,255,0);    server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}")); });
  server.on("/api/snooze",      HTTP_GET, [](){ bool ok=sendCommand(3,255,0);    server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}")); });
  server.on("/api/clear_snooze",HTTP_GET, [](){ bool ok=sendCommand(4,255,0);    server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}")); });

  server.begin();
  Serial.println("HTTP server started on port 80.");
  if (WiFi.status()==WL_CONNECTED) {
    Serial.printf("Open http://%s/  (WiFi channel: %d)\n", WiFi.localIP().toString().c_str(), WiFi.channel());
  }
}

// ================== Loop ==================
void loop() {
  server.handleClient();

  // Power save diagnostic check (every 30s)
  static uint32_t lastPowerSaveCheck = 0;
  if (millis() - lastPowerSaveCheck > 30000) {
    wifi_ps_type_t ps_type;
    esp_err_t ps_get_result = esp_wifi_get_ps(&ps_type);
    if (ps_get_result == ESP_OK) {
      Serial.printf("[PS-CHECK] Power save mode: %d (0=NONE)\n", ps_type);
    } else {
      Serial.printf("[PS-CHECK] Failed to get power save mode: %d\n", ps_get_result);
    }
    lastPowerSaveCheck = millis();
  }

  // Heartbeat
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    Serial.printf("[beat] NTP %s | CH %d | IP %s\n",
      ntpSynced()? "synced":"not-synced",
      WiFi.channel(),
      WiFi.localIP().toString().c_str());
  }
}
