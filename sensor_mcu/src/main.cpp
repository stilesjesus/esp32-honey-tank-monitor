// main.cpp — Sensor MCU (battery) - Robust Version
// Role: scan 5 s -> median -> send to Siren + Webserver via ESP-NOW -> deep sleep 120 s

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_now.h>
#include <esp_wifi.h>
extern "C" {
  #include "esp_bt.h"
}

// ================== Hardware: A02YYUW ==================
#define A02YYUW_TX 18
#define A02YYUW_RX 5
HardwareSerial sensorSerial(2);

// ================== Identity ==================
#ifndef TANK_ID
#define TANK_ID 2
#endif

// ================== Peers (use STA MACs you provided) ==================
static const uint8_t MAC_SIREN[6]     = {0x00,0x00,0x00,0x00,0x00,0x00}; // Replace with actual MAC
static const uint8_t MAC_WEBSERVER[6] = {0x00,0x00,0x00,0x00,0x00,0x00}; // Replace with actual MAC

// ================== Timing ==================
static const uint32_t SCAN_MS     = 5000;
static const uint32_t JITTER_MS   = 2000;
static const uint64_t SLEEP_US    = 120ULL * 1000ULL * 1000ULL;

// ================== Packet format ==================
#pragma pack(push,1)
struct SensorPacket {
  uint8_t  ver;
  uint8_t  tank_id;
  uint16_t distance_mm;
  uint16_t battery_mV;
  uint8_t  flags;
  uint8_t  crc8;
};
#pragma pack(pop)

static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t c = 0x00;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) {
      c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
  }
  return c;
}

// ================== Sampling ==================
static const int MAX_SAMPLES = 100;
static float samples[MAX_SAMPLES];
static int   sampleCount = 0;

static bool readA02YYUW(float &distance_cm) {
  while (sensorSerial.available() >= 4) {
    uint8_t b0 = sensorSerial.read();
    if (b0 != 0xFF) continue;

    if (sensorSerial.available() < 3) return false;
    uint8_t b1 = sensorSerial.read();
    uint8_t b2 = sensorSerial.read();
    uint8_t b3 = sensorSerial.read();

    if (((uint8_t)(b0 + b1 + b2)) != b3) continue;

    int raw_mm = (b1 << 8) | b2;
    if (raw_mm < 30 || raw_mm > 4500) continue;

    distance_cm = raw_mm / 10.0f;
    return true;
  }
  return false;
}

static void sortSmall(float *arr, int n) {
  for (int i = 0; i < n - 1; ++i) {
    int m = i;
    for (int j = i + 1; j < n; ++j) if (arr[j] < arr[m]) m = j;
    if (m != i) { float t = arr[i]; arr[i] = arr[m]; arr[m] = t; }
  }
}

static float computeMedian(const float *arr, int n) {
  if (n <= 0) return NAN;
  static float tmp[MAX_SAMPLES];
  for (int i = 0; i < n; ++i) tmp[i] = arr[i];
  sortSmall(tmp, n);
  return (n & 1) ? tmp[n/2] : 0.5f * (tmp[n/2 - 1] + tmp[n/2]);
}

// ================== Battery ==================
#define BATTERY_ADC_PIN   -1
#define ADC_REF_VOLTAGE   3300.0
#define ADC_RESOLUTION    4096.0
#define DIVIDER_RATIO_X10 20

static uint16_t readBatteryMilliVolts() {
#if (BATTERY_ADC_PIN >= 0)
  int adc = analogRead(BATTERY_ADC_PIN);
  float v_mV = (adc * (ADC_REF_VOLTAGE / ADC_RESOLUTION)) * (DIVIDER_RATIO_X10 / 10.0f);
  if (v_mV < 0) v_mV = 0;
  if (v_mV > 65535) v_mV = 65535;
  return (uint16_t)(v_mV + 0.5f);
#else
  return 0;
#endif
}

// ================== ESP-NOW sending (simplified) ==================
volatile bool g_sendDone = false;
volatile bool g_sendOk   = false;

static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  g_sendOk = (status == ESP_NOW_SEND_SUCCESS);
  g_sendDone = true;
}

static bool addPeer(const uint8_t mac[6], uint8_t channel) {
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel;
  peer.encrypt = false;
  esp_err_t result = esp_now_add_peer(&peer);
  Serial.printf("addPeer CH%d: %s\n", channel, (result == ESP_OK) ? "OK" : "FAILED");
  return (result == ESP_OK);
}

static bool sendPacketTo(const uint8_t mac[6], const SensorPacket &pkt, uint8_t target_channel) {
  // Try to set channel - don't crash if it fails
  esp_err_t ch_result = esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Channel %d: %s\n", target_channel, (ch_result == ESP_OK) ? "OK" : "FAILED");
  
  // Brief settle time
  delay(20);
  
  g_sendDone = false; 
  g_sendOk = false;
  
  esp_err_t send_result = esp_now_send(mac, (const uint8_t*)&pkt, sizeof(pkt));
  if (send_result != ESP_OK) {
    Serial.printf("Send failed: %d\n", send_result);
    return false;
  }

  // Wait for callback with timeout
  uint32_t start = millis();
  while (!g_sendDone && (millis() - start) < 300) {
    delay(1);
  }
  
  Serial.printf("Result: %s (%dms)\n", g_sendOk ? "OK" : "FAILED", millis() - start);
  return g_sendOk;
}

// Simple channel discovery - don't crash on failure
static uint8_t findWebserverChannel() {
  Serial.println("Scanning networks...");
  
  // Default fallback
  uint8_t channel = 1;
  
  // Try scanning - catch any failures
  int n = WiFi.scanNetworks(false, false, false, 200);
  
  if (n > 0) {
    Serial.printf("Found %d networks\n", n);
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == "SpectrumSetup-B5") {
        channel = WiFi.channel(i);
        Serial.printf("Target network on CH%d\n", channel);
        break;
      }
    }
    WiFi.scanDelete();
  } else {
    Serial.printf("Scan failed or no networks, using CH%d\n", channel);
  }
  
  return channel;
}

static void safeRadiosOff() {
  Serial.println("Disabling radios...");
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  btStop();
}

// ================== Main App ==================
void setup() {
  Serial.begin(115200);
  delay(500);  // Give serial time to initialize

  // Start with radios off
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(100);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\n=== SENSOR %d START ===\n", TANK_ID);
  Serial.printf("Wake: %s\n", (cause == ESP_SLEEP_WAKEUP_TIMER) ? "timer" : "reset");

  // === SAMPLING PHASE ===
  Serial.println("Starting sensor sampling...");
  sensorSerial.begin(9600, SERIAL_8N1, A02YYUW_RX, A02YYUW_TX);
  
  sampleCount = 0;
  uint32_t startTime = millis();
  
  while ((millis() - startTime) < SCAN_MS && sampleCount < MAX_SAMPLES) {
    float dcm;
    if (readA02YYUW(dcm)) {
      samples[sampleCount++] = dcm;
      if (sampleCount % 10 == 0) {
        Serial.printf("Samples: %d\n", sampleCount);
      }
    } else {
      delay(10);
    }
    
    // Watchdog yield
    yield();
  }

  float median_cm = NAN;
  if (sampleCount > 0) {
    median_cm = computeMedian(samples, sampleCount);
  }

  Serial.printf("Samples=%d, median=%.1fcm\n", sampleCount, median_cm);

  // Jitter delay
  uint32_t jitter = esp_random() % (JITTER_MS + 1);
  Serial.printf("Jitter: %dms\n", jitter);
  delay(jitter);

  // Prepare packet
  SensorPacket pkt{};
  pkt.ver         = 1;
  pkt.tank_id     = (uint8_t)TANK_ID;
  bool valid      = (sampleCount > 0) && isfinite(median_cm);
  uint16_t distmm = valid ? (uint16_t)max(0, min(65535, (int)(median_cm * 10 + 0.5f))) : 0;
  pkt.distance_mm = distmm;
  pkt.battery_mV  = readBatteryMilliVolts();
  pkt.flags       = 0;
  if (valid) pkt.flags |= 0x01;
  if (valid && (median_cm <= 6.0f)) pkt.flags |= 0x02;
  pkt.crc8        = crc8((uint8_t*)&pkt, sizeof(pkt) - 1);

  Serial.printf("Packet ready: dist=%dmm flags=0x%02X\n", pkt.distance_mm, pkt.flags);

  // === TRANSMISSION PHASE ===
  Serial.println("\n=== TRANSMISSION ===");
  
  // Initialize WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  
  delay(100);  // Let WiFi stabilize

  // Find webserver channel (with error protection)
  uint8_t webserver_channel = findWebserverChannel();
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
  } else {
    Serial.println("ESP-NOW OK");
    esp_now_register_send_cb(onDataSent);
    
    // Add peers
    bool peers_ok = true;
    peers_ok &= addPeer(MAC_SIREN, 1);
    peers_ok &= addPeer(MAC_WEBSERVER, webserver_channel);
    
    if (peers_ok) {
      delay(50);  // Brief settle time
      
      // Send to siren first
      Serial.println("\n--- SIREN ---");
      bool siren_ok = sendPacketTo(MAC_SIREN, pkt, 1);
      if (!siren_ok) {
        delay(100);
        siren_ok = sendPacketTo(MAC_SIREN, pkt, 1);
      }
      
      // Send to webserver
      Serial.println("\n--- WEBSERVER ---");
      bool web_ok = sendPacketTo(MAC_WEBSERVER, pkt, webserver_channel);
      if (!web_ok) {
        delay(100);
        web_ok = sendPacketTo(MAC_WEBSERVER, pkt, webserver_channel);
      }
      
      Serial.printf("\nSUMMARY: Siren=%s Web=%s\n", 
        siren_ok ? "OK" : "FAIL", web_ok ? "OK" : "FAIL");
    } else {
      Serial.println("Peer setup failed");
    }
  }

  // === SLEEP ===
  Serial.println("\n=== SLEEP ===");
  safeRadiosOff();
  
  Serial.println("Sleeping 120s...");
  Serial.flush();
  
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

void loop() {
  // Never reached due to deep sleep
}
