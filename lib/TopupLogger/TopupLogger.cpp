#include "TopupLogger.h"
#include "ArduinoJson.h"
#include "WebSocketLogger.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

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

  // Use StaticJsonDocument to create the JSON payload
  StaticJsonDocument<200> doc;
  doc["runTimeMillis"] = runTimeMillis;
  doc["weightIncrement"] = weightIncrement;

  String payload;
  serializeJson(doc, payload);

  // Create an async HTTP request
  AsyncClient *client = new AsyncClient();
  client->onError([this](void *arg, AsyncClient *client, int error) {
    if (_logger)
      _logger->println(String("Connect Error: ") +
                       client->errorToString(error));
    delete client;
  });

  client->onConnect([this, payload](void *arg, AsyncClient *client) {
    Serial.println("Connected");
    client->onData(
        [this](void *arg, AsyncClient *client, void *data, size_t len) {
          if (this->_logger)
            this->_logger->println(String("[topuplogger] Received response: ") +
                                   (char *)data);
          client->close();
          delete client;
        });

    String request = "POST /api/log HTTP/1.1\r\n";
    request += "Host: " + String(client->remoteIP().toString()) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + String(payload.length()) + "\r\n";
    request += "\r\n" + payload;

    client->write(request.c_str());
  });

  if (!client->connect(IPAddress(192, 168, 0, 112), 8000)) {
    if (_logger)
      _logger->println("Connect failed");
    delete client;
  }
}
