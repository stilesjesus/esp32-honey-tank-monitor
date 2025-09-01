// MAC Address Finder Utility
// Upload this sketch to each ESP32 to discover its MAC addresses
// Use the STA MAC address in your project configuration

#include <Arduino.h>
#include <WiFi.h>
extern "C" {
  #include "esp_system.h"
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n" + String("=").substring(0, 50));
  Serial.println("ESP32 MAC Address Finder");
  Serial.println(String("=").substring(0, 50));
  
  // Initialize WiFi to get MAC addresses
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Get and display MAC addresses
  String staMac = WiFi.macAddress();
  uint8_t baseMac[6];
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
  
  Serial.println("\nMAC ADDRESSES:");
  Serial.println("--------------");
  Serial.printf("STA MAC (WiFi Station): %s\n", staMac.c_str());
  Serial.printf("Formatted for code:     {0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X}\n", 
    baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  
  // Also show AP MAC for reference
  uint8_t apMac[6];
  esp_read_mac(apMac, ESP_MAC_WIFI_SOFTAP);
  Serial.printf("AP MAC (Access Point):  %02X:%02X:%02X:%02X:%02X:%02X\n", 
    apMac[0], apMac[1], apMac[2], apMac[3], apMac[4], apMac[5]);
  
  Serial.println("\nIMPORTANT:");
  Serial.println("----------");
  Serial.println("Use the STA MAC address in your project files:");
  Serial.println("- Update MAC_SENSORS array in siren-mcu and webserver-mcu");
  Serial.println("- Update MAC_SIREN and MAC_WEBSERVER in sensor-mcu");
  Serial.println("- Each ESP32 has a unique MAC address");
  
  Serial.println("\nCHIP INFO:");
  Serial.println("----------");
  Serial.printf("Chip Model: %s\n", ESP.getChipModel());
  Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());
  Serial.printf("Flash Size: %d bytes\n", ESP.getFlashChipSize());
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  
  Serial.println(String("=").substring(0, 50));
  Serial.println("Keep this information for project setup!");
  Serial.println(String("=").substring(0, 50));
}

void loop() {
  // Flash built-in LED to show the sketch is running
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  if (millis() - lastBlink > 1000) {
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
    lastBlink = millis();
    
    // Reminder every 10 seconds
    static int counter = 0;
    if (++counter >= 10) {
      Serial.println("TIP: Copy the STA MAC address above for your project setup");
      counter = 0;
    }
  }
}
