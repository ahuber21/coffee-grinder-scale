#include <ADS1232.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

// needs to be included after WiFiManager.h
// which does not properly protect some defines
#include <ESPAsyncWebServer.h>

#include "defines.h"
#include "wslogger.h"

ADS1232 scale;
AsyncWebServer server(SERVER_PORT);
WSLogger ws;

float scale_units = 0.0f;
uint32_t scale_refresh_millis = 0;

void setup() {
  Serial.begin(115200);

  WiFiManager wifiManager;
  wifiManager.autoConnect("Eureka setup");

  server.begin();
  ws.begin(&server);
  ws.println("WebSocket ready");

  ArduinoOTA.begin();
  ws.print("Arduino OTA ready at ");
  ws.println(WiFi.localIP());

  scale.begin(SCALE_DOUT, SCALE_SCLK, SCALE_PDWN, SCALE_SPEED);
  scale.tare();
  ws.println("Scale ready");
}

void loop() {
  ArduinoOTA.handle();

  float last = scale_units;
  scale.get_units(scale_units);
  if (scale_units != last || millis() - scale_refresh_millis > 1000) {
    ws.println(scale_units);
    scale_refresh_millis = millis();
  }
}
