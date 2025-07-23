#include "ProgressLogger.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include "ArduinoJson.h"
#include "WebSocketLogger.h"

namespace proglog::detail {
void fillRequest(const String &endpoint, AsyncClient *client,
                 const String &payload, String &request) {
  if (!client || endpoint.isEmpty() || payload.isEmpty()) {
    return;
  }
  request = "POST " + endpoint + " HTTP/1.1\r\n";
  request += "Host: " + String(client->remoteIP().toString()) + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(payload.length()) + "\r\n";
  request += "\r\n" + payload;
}
} // namespace proglog::detail

ProgressLogger::ProgressLogger()
    : _endpoint(""), _logger(nullptr), _sending(false),
      _sending_start_millis(0), _max_buffer_size(100) {}
void ProgressLogger::begin(const char *endpoint, const WebSocketLogger *logger,
                           unsigned long minLogInterval) {
  _endpoint = endpoint;
  _logger = logger;
  _min_log_interval = minLogInterval;
  _sending = false;
  _sending_start_millis = 0;
}

void ProgressLogger::logData(unsigned long runTimeMillis, float weight) {
  auto last = _buffer.empty() ? 0 : _buffer.back().runTimeMillis;

  if (last > 0 && (runTimeMillis < last + _min_log_interval)) {
    // Skip logging if the interval is too short
    return;
  }

  if (_sending) {
    // If currently sending, wait for the next update to send data
    if (_logger)
      _logger->println("Currently sending, skipping log entry");
    return;
  }

  if (_buffer.size() >= _max_buffer_size) {
    // If buffer is full, send data immediately
    sendBufferedData();
  }
  _buffer.push_back({runTimeMillis, weight});
}

void ProgressLogger::handleSendingTimeout() {
  if (!_sending)
    return;
  if (millis() - _sending_start_millis > SEND_TIMEOUT) {
    if (_logger)
      _logger->println("Sending timeout, clearing buffer");
    _buffer.clear();
    _sending = false;
  }
}

void ProgressLogger::update() {
  if (_sending)
    handleSendingTimeout();
  if (_buffer.empty())
    return;
  auto last = _buffer.back().runTimeMillis;
  if (millis() - last < SEND_INTERVAL)
    return;
  sendBufferedData();
}

void ProgressLogger::sendBufferedData() {
  _sending = true;

  // Reset last log time after sending
  if (_buffer.empty()) {
    _sending = false;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    if (_logger)
      _logger->println("WiFi not connected");
    _buffer.clear();
    _sending = false;
    return;
  }

  StaticJsonDocument<200> doc;

  // Send each entry, set _sending false after last client closes
  size_t pending = _buffer.size();

  for (const auto &entry : _buffer) {
    doc["runTimeMillis"] = entry.runTimeMillis;
    doc["weight"] = entry.weight;
    String payload;
    serializeJson(doc, payload);

    AsyncClient *client = new AsyncClient();
    client->onError(
        [this, &pending](void *arg, AsyncClient *client, int error) {
          if (_logger)
            _logger->println(String("Connect Error: ") +
                             client->errorToString(error));
          delete client;
          pending--;
          if (pending == 0) {
            this->_sending = false;
            this->_buffer.clear();
          }
        });

    client->onConnect([this, payload, &pending](void *arg,
                                                AsyncClient *client) {
      client->onData([this, &pending](void *arg, AsyncClient *client,
                                      void *data, size_t len) {
        if (this->_logger)
          this->_logger->println(String("[ProgessLogger] Received response: ") +
                                 (char *)data);
        client->close();
        delete client;
        pending--;
        if (pending == 0) {
          this->_sending = false;
          this->_buffer.clear();
        }
      });

      String request;
      proglog::detail::fillRequest(_endpoint, client, payload, request);

      client->write(request.c_str());
    });

    if (!client->connect(IPAddress(192, 168, 0, 112), 8000)) {
      if (_logger)
        _logger->println("Connect failed");
      delete client;
      pending--;
      if (pending == 0) {
        this->_sending = false;
        this->_buffer.clear();
      }
    }
  }
}