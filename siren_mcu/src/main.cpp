/* ============================================================================
Important Security & Publishing Notes
  • Do NOT commit secrets:
      - Wi-Fi SSID/PASSWORD
      - ESP-NOW keys (PMK/LMK)
      - Real peer MAC addresses (optional, but prefer to keep in a private file)

    Keep them in a gitignored header, e.g. `secrets.h`:
         secrets.h  (DO NOT COMMIT)
        #pragma once
        Example placeholders; fill with your real values
        static const char* WIFI_SSID = "…";
        static const char* WIFI_PASS = "…";
        static const uint8_t PMK[16] = { // 16 bytes  };
        static const uint8_t MAC_SIREN[6]   = { // STA MAC  };
        static const uint8_t MAC_SENSORS[3][6] = { // STA MACs };
    .gitignore:
        secrets.h

  • MAC allow-lists help but can be spoofed.
    For authenticity/integrity:
      - Enable ESP-NOW encryption (PMK + per-peer LMK), and/or
      - Add an application-layer authenticator (e.g., HMAC/CMAC over your packet).
        Replace plain CRC with HMAC if you need tamper resistance.

    (Reference sketch)
        // Set global PMK once:
        // esp_now_set_pmk(PMK);

        // For each peer:
        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, MAC_SIREN, 6);
        peer.channel = 0;          // current channel
        peer.encrypt = true;       // IMPORTANT
        // memcpy(peer.lmk, LMK_FOR_THIS_PEER, 16);  // per-peer 16-byte key
        esp_now_add_peer(&peer);

  • Avoid publishing raw serial logs that include your device MACs if you consider
    them sensitive in your context (they’re not credentials, but still identifiers).
============================================================================ */


// main.cpp — Siren MCU (always-on listener + 5s pulse + 5min per-tank snooze)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>  // Added for channel control
#include <string.h> // memcmp

// ====== Hardware ======
static const int SIREN_PIN = 25;      // IRLZ44N gate, low-side. HIGH=ON.
static const uint32_t SIREN_ON_MS = 5000;       // 5 s pulse
static const uint32_t SNOOZE_MS   = 5UL * 60UL * 1000UL; // 5 min per tank
static const uint32_t STALE_MS    = 7UL * 60UL * 1000UL; // ignore >7 min old tanks
static const float    TRIGGER_CM  = 6.0f;       // alarm threshold

// ====== IDs / MACs (STA MACs you provided) ======
static const uint8_t MAC_WEBSERVER[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t MAC_SENSORS[3][6] = {
  {0x00,0x00,0x00,0x00,0x00,0x00}, // Tank 0
  {0x00,0x00,0x00,0x00,0x00,0x00}, // Tank 1
  {0x00,0x00,0x00,0x00,0x00,0x00}  // Tank 2
};

// ====== Packet formats (match sensor/webserver) ======
#pragma pack(push,1)
struct SensorPacket {
  uint8_t  ver;          // 1
  uint8_t  tank_id;      // 0/1/2
  uint16_t distance_mm;  // median over 5 s (0 if invalid)
  uint16_t battery_mV;   // may be 0
  uint8_t  flags;        // bit0: valid_median, bit1: at_risk_le_6cm
  uint8_t  crc8;         // CRC-8 over [ver..flags]
};

// Optional control from Webserver -> Siren
// type = 0xC1 marks command packet
// cmd: 1=FORCE_ON (ms), 2=FORCE_OFF, 3=SNOOZE_5MIN, 4=CLEAR_SNOOZE, 5=SNOOZE_CUSTOM_MS
struct CommandPacket {
  uint8_t  ver;       // 1
  uint8_t  type;      // 0xC1
  uint8_t  cmd;       // 1..5
  uint8_t  tank_id;   // 0/1/2 or 255 for ALL
  uint16_t ms;        // duration for FORCE_ON or custom snooze duration
  uint8_t  crc8;      // CRC-8 over [ver..ms]
};
#pragma pack(pop)

// CRC-8-ATM (poly 0x07, init 0x00)
static uint8_t crc8(const uint8_t* d, size_t n) {
  uint8_t c = 0;
  for (size_t i=0;i<n;i++){ c ^= d[i]; for(int b=0;b<8;b++) c = (c&0x80)? (uint8_t)((c<<1)^0x07):(uint8_t)(c<<1); }
  return c;
}

// ====== Siren control (non-blocking) ======
static bool sirenActive = false;
static uint32_t sirenOffAt = 0;

inline void sirenOn()  { digitalWrite(SIREN_PIN, HIGH); }
inline void sirenOff() { digitalWrite(SIREN_PIN, LOW);  }

static void sirenPulse(uint32_t on_ms) {
  Serial.printf("SIREN ON for %dms\n", on_ms);
  sirenOn();
  sirenActive = true;
  sirenOffAt = millis() + on_ms;
}

// ====== Per-tank state ======
static const int MAX_TANKS = 3;
static float    lastDistanceCm[MAX_TANKS] = {NAN,NAN,NAN};
static uint32_t lastRxMs[MAX_TANKS]       = {0,0,0};
static uint32_t snoozeUntilMs[MAX_TANKS]  = {0,0,0};

// ====== Helpers ======
static bool macEquals(const uint8_t *a, const uint8_t *b) { return memcmp(a,b,6)==0; }
static bool isFromKnownSensor(const uint8_t *mac, int &tankIdOut) {
  for (int i=0;i<MAX_TANKS;i++){ if (macEquals(mac, MAC_SENSORS[i])) { tankIdOut = i; return true; } }
  return false;
}
static bool isFromWebserver(const uint8_t *mac) { return macEquals(mac, MAC_WEBSERVER); }

static void applySnooze(int tankId, uint32_t nowMs, uint32_t addMs=SNOOZE_MS) {
  if (tankId>=0 && tankId<MAX_TANKS) {
    snoozeUntilMs[tankId] = nowMs + addMs;
    Serial.printf("Tank %d snoozed for %d minutes\n", tankId, addMs / (60 * 1000));
  }
}
static void clearSnooze(int tankId) {
  if (tankId==255) { 
    for(int i=0;i<MAX_TANKS;i++) {
      snoozeUntilMs[i]=0;
      Serial.printf("Tank %d snooze cleared\n", i);
    }
  }
  else if (tankId>=0 && tankId<MAX_TANKS) {
    snoozeUntilMs[tankId]=0;
    Serial.printf("Tank %d snooze cleared\n", tankId);
  }
}

// ====== Core decision: handle a sensor update ======
static void handleSensorPacket(const SensorPacket &p) {
  if (p.ver != 1) {
    Serial.printf("Wrong packet version: %d\n", p.ver);
    return;
  }
  
  // verify crc
  uint8_t calc_crc = crc8((const uint8_t*)&p, sizeof(p)-1);
  if (p.crc8 != calc_crc) {
    Serial.printf("CRC mismatch: expected %02X got %02X\n", calc_crc, p.crc8);
    return;
  }

  const uint8_t tid = p.tank_id;
  if (tid >= MAX_TANKS) {
    Serial.printf("Invalid tank ID: %d\n", tid);
    return;
  }

  const bool valid = (p.flags & 0x01) && p.distance_mm>0;
  const float d_cm = valid ? (p.distance_mm / 10.0f) : NAN;

  const uint32_t now = millis();
  lastRxMs[tid] = now;
  lastDistanceCm[tid] = d_cm;

  Serial.printf("Tank %d: distance=%.1fcm battery=%dmV valid=%s ", 
    tid, d_cm, p.battery_mV, valid ? "YES" : "NO");

  if (!valid) {
    Serial.println("(invalid data)");
    return;
  }

  const bool atRisk = (d_cm <= TRIGGER_CM);
  Serial.printf("at_risk=%s ", atRisk ? "YES" : "NO");

  if (atRisk) {
    // Check per-tank snooze
    if (now >= snoozeUntilMs[tid]) {
      Serial.println("-> TRIGGERING SIREN");
      
      // If siren already active (due to another tank), piggyback: set snooze for this tank too.
      if (!sirenActive) {
        sirenPulse(SIREN_ON_MS);
      } else {
        Serial.println("(siren already active, applying snooze)");
      }
      applySnooze(tid, now, SNOOZE_MS);
    } else {
      uint32_t snooze_remaining = snoozeUntilMs[tid] - now;
      Serial.printf("(snoozed for %d more seconds)\n", snooze_remaining / 1000);
    }
  } else {
    Serial.println("(safe level)");
  }
}

// ====== Handle a command from the webserver (optional) ======
static void handleCommandPacket(const CommandPacket &c) {
  Serial.printf("Command received: ver=%d type=0x%02X cmd=%d tank=%d ms=%d\n",
    c.ver, c.type, c.cmd, c.tank_id, c.ms);
    
  if (c.ver != 1 || c.type != 0xC1) {
    Serial.println("Invalid command header");
    return;
  }
  
  uint8_t calc_crc = crc8((const uint8_t*)&c, sizeof(c)-1);
  if (c.crc8 != calc_crc) {
    Serial.printf("Command CRC mismatch: expected %02X got %02X\n", calc_crc, c.crc8);
    return;
  }

  const uint8_t cmd = c.cmd;
  const uint8_t tid = c.tank_id;
  const uint32_t now = millis();

  switch (cmd) {
    case 1: { // FORCE_ON for ms (cap at 10s for safety)
      uint32_t dur = c.ms;
      if (dur == 0 || dur > 10000) dur = SIREN_ON_MS;
      Serial.printf("Force ON for %dms\n", dur);
      sirenPulse(dur);
      // Optional: set snooze for target/all tanks so it doesn't immediately retrigger
      if (tid==255) {
        for(int i=0;i<MAX_TANKS;i++) applySnooze(i, now);
      } else {
        applySnooze(tid, now);
      }
    } break;
    
    case 2: { // FORCE_OFF immediately
      Serial.println("Force OFF");
      sirenOff();
      sirenActive = false;
      // Optionally also snooze to avoid immediate re-alarm if still at risk:
      if (tid==255) {
        for(int i=0;i<MAX_TANKS;i++) applySnooze(i, now);
      } else {
        applySnooze(tid, now);
      }
    } break;
    
    case 3: { // SNOOZE_5MIN
      Serial.println("Snooze 5 minutes");
      if (tid==255) {
        for(int i=0;i<MAX_TANKS;i++) applySnooze(i, now);
      } else {
        applySnooze(tid, now);
      }
    } break;
    
    case 4: { // CLEAR_SNOOZE
      Serial.println("Clear snooze");
      clearSnooze(tid);
    } break;
    
    case 5: { // SNOOZE_CUSTOM_MS - use the ms field as snooze duration
      uint32_t customMs = c.ms;
      if (customMs == 0) customMs = SNOOZE_MS; // fallback to 5min default if 0
      if (customMs > 60UL * 60UL * 1000UL) customMs = 60UL * 60UL * 1000UL; // cap at 1 hour
      Serial.printf("Custom snooze for %d minutes\n", customMs / (60 * 1000));
      
      if (tid==255) {
        for(int i=0;i<MAX_TANKS;i++) applySnooze(i, now, customMs);
      } else {
        applySnooze(tid, now, customMs);
      }
    } break;
    
    default:
      Serial.printf("Unknown command: %d\n", cmd);
      break;
  }
}

// ====== ESP-NOW receive callback ======
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  Serial.printf("ESP-NOW RX from %02X:%02X:%02X:%02X:%02X:%02X len=%d: ",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], len);
  
  // Accept only from known sensors or webserver
  int sensorTid = -1;
  const bool fromSensor   = isFromKnownSensor(mac, sensorTid);
  const bool fromWeb      = isFromWebserver(mac);

  if (!fromSensor && !fromWeb) {
    Serial.println("REJECTED (unknown sender)");
    return;
  }

  if (fromSensor) {
    Serial.printf("SENSOR %d ", sensorTid);
  } else {
    Serial.print("WEBSERVER ");
  }

  if (len == (int)sizeof(SensorPacket)) {
    Serial.println("(SensorPacket)");
    SensorPacket p;
    memcpy(&p, data, sizeof(p));
    // If it claims a tank id, also ensure it matches the sender we expect:
    if (fromSensor && p.tank_id != (uint8_t)sensorTid) {
      Serial.printf("Tank ID mismatch: MAC suggests %d but packet claims %d\n", sensorTid, p.tank_id);
      return;
    }
    handleSensorPacket(p);
  }
  else if (len == (int)sizeof(CommandPacket) && fromWeb) {
    Serial.println("(CommandPacket)");
    CommandPacket c;
    memcpy(&c, data, sizeof(c));
    handleCommandPacket(c);
  }
  else {
    Serial.printf("REJECTED (wrong size: expected %d or %d)\n", sizeof(SensorPacket), sizeof(CommandPacket));
  }
}

// ====== Setup & loop ======
void setup() {
  pinMode(SIREN_PIN, OUTPUT);
  sirenOff();

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== SIREN MCU STARTING ===");

  // Initialize WiFi in STA mode and set to channel 1
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);  // Don't erase stored credentials, but disconnect
  
  // Set to fixed channel 1 (same as webserver network)
  esp_err_t ch_result = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Set WiFi channel to 1: %s\n", (ch_result == ESP_OK) ? "OK" : "FAILED");
  
  // Verify channel
  uint8_t actual_channel;
  wifi_second_chan_t second_chan;
  esp_wifi_get_channel(&actual_channel, &second_chan);
  Serial.printf("Current WiFi channel: %d\n", actual_channel);
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    // keep running; siren stays off
  } else {
    esp_err_t cb_result = esp_now_register_recv_cb(onDataRecv);
    Serial.printf("ESP-NOW callback registered: %s\n", (cb_result == ESP_OK) ? "OK" : "FAILED");
    Serial.println("ESP-NOW ready - listening for packets");
  }
  
  Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
  Serial.println("Ready to receive sensor data and webserver commands");
}

void loop() {
  const uint32_t now = millis();

  // Non-blocking siren auto-off after pulse
  if (sirenActive && (int32_t)(now - sirenOffAt) >= 0) {
    Serial.println("SIREN OFF (timeout)");
    sirenOff();
    sirenActive = false;
  }

  // Optional diagnostic output every 30 seconds
  static uint32_t lastDiag = 0;
  if (now - lastDiag > 30000) {
    lastDiag = now;
    Serial.printf("[DIAG] Siren: %s | ", sirenActive ? "ACTIVE" : "off");
    for (int i = 0; i < MAX_TANKS; i++) {
      if (lastRxMs[i] == 0) {
        Serial.printf("T%d:never ", i);
      } else {
        uint32_t age_s = (now - lastRxMs[i]) / 1000;
        uint32_t snooze_remain = (snoozeUntilMs[i] > now) ? (snoozeUntilMs[i] - now) / 1000 : 0;
        Serial.printf("T%d:%.1fcm(%ds ago,snz:%ds) ", i, lastDistanceCm[i], age_s, snooze_remain);
      }
    }
    Serial.println();
  }

  delay(10); // small yield
}
