#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "secrets.h"

uint32_t seconds = 0;

void setup() {
  Serial.begin(115200);
  WiFiManager wifiManager;
  wifiManager.autoConnect(WIFI_MANAGER_SSID);

  ArduinoOTA.begin();
  Serial.print("Arduino OTA ready at ");
  Serial.println(WiFi.localIP());
}

void loop() {
  ArduinoOTA.handle();

  if (millis() / 1000 > seconds) {
    Serial.println("Hi!");
    seconds = millis() / 1000;
  }
}
