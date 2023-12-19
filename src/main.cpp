#include <ADS1232.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

// needs to be included after WiFiManager.h
// which does not properly protect some defines
#include <ESPAsyncWebServer.h>
#include <WebSocketLogger.h>
#include <WebSocketSettings.h>

#include "defines.h"

ADS1232 scale =
    ADS1232(ADC_PDWN_PIN, ADC_SCLK_PIN, ADC_DOUT_PIN, ADC_A0_PIN, ADC_SPEED_PIN,
            ADC_GAIN1_PIN, ADC_GAIN0_PIN, ADC_TEMP_PIN);
AsyncWebServer server(SERVER_PORT);
WebSocketLogger logger;
WebSocketSettings settings;

int32_t scale_raw = 0;
uint32_t scale_refresh_millis = 0;

// WebSettings settings;

void setup() {
  Serial.begin(115200);

  WiFiManager wifiManager;
  wifiManager.autoConnect("Eureka setup");

  server.begin();
  logger.begin(&server);
  logger.println("WebSocket ready");

  settings.begin(&server, &logger);
  logger.println("Settings ready");

  ArduinoOTA.begin();
  logger.print("Arduino OTA ready at ");
  logger.println(WiFi.localIP());

  // power up the scale circuit
  pinMode(ADC_LDO_EN_PIN, OUTPUT);
  digitalWrite(ADC_LDO_EN_PIN, HIGH);
  scale.begin();
  scale.tare();
  logger.println("Scale ready");
}

void loop() {
  ArduinoOTA.handle();

  uint32_t last = scale_raw;
  scale_raw = scale.readRaw(settings.scale.read_samples);
  if (scale_raw != last || millis() - scale_refresh_millis > 1000) {
    logger.println(scale_raw);
    scale_refresh_millis = millis();
  }
}
