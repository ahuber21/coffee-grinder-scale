/**

#pragma once
#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <wslogger.h>

#define LOG(message)                                                           \
  if (_logger) {                                                               \
    _logger->print(message);                                                   \
  } else {                                                                     \
    Serial.print(message);                                                     \
  }

class WebSettings {
public:
  struct Scale {
    byte read_samples = 8;
  };

  WebSettings() : _ws("/ws-settings") {}
  void begin(AsyncWebServer *srv, const WSLogger * = nullptr);

  Scale scale;

  void print(const String &message) { LOG(message); }
  void println(const String &message) const { LOG(message); }
  template <typename T> void print(T message) { LOG(message); }
  template <typename T> void println(T message) { LOG(message); }

private:
  AsyncWebServer *_server;
  AsyncWebSocket _ws;
  const WSLogger *_logger;
};

*/