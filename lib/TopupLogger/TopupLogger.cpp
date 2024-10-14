#include "TopupLogger.h"
#include "ArduinoJson.h"
#include "WebSocketLogger.h"
#include <HTTPClient.h>

TopupLogger::TopupLogger() : _endpoint(nullptr), _logger(nullptr) {}

void TopupLogger::begin(const char *endpoint, const WebSocketLogger *logger) {
  _endpoint = endpoint;
  _logger = logger;
}

void TopupLogger::logTopupData(unsigned long runTimeMillis,
                               float weightIncrement) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  HTTPClient http;
  http.begin(_endpoint); // Your endpoint URL

  http.addHeader("Content-Type", "application/json");

  // Use StaticJsonDocument to create the JSON payload
  StaticJsonDocument<200> doc;
  doc["runTimeMillis"] = runTimeMillis;
  doc["weightIncrement"] = weightIncrement;

  // Send POST request
  String payload;
  serializeJson(doc, payload);

  int httpResponseCode = http.POST(payload);
  http.end();

  if (_logger) {
    if (httpResponseCode > 0) {
      _logger->println(
          String("[topuplogger] POST request sent. Response code: ") +
          httpResponseCode);
    } else {
      _logger->println(
          String(
              "[topuplogger] Error sending POST request. Response code: %d") +
          httpResponseCode);
    }
  }
}
