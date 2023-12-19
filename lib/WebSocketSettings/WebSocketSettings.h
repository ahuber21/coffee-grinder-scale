#pragma once
#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <EEPROM.h>

class WebSocketLogger;

class WebSocketSettings {
public:
  struct Scale {
    byte read_samples = 8;
  };

  WebSocketSettings();
  void begin(AsyncWebServer *srv, const WebSocketLogger *logger);
  void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data,
                        size_t len);

  Scale scale;

private:
  void loadScaleFromEEPROM();
  void saveScaleToEEPROM();

  bool handleWebSocketText(const String &cmd, String &response);

  // EEPROM address to store the scale value
  static const int EEPROM_SCALE_ADDRESS;

  AsyncWebSocket _ws;
  AsyncWebServer *_server;
  const WebSocketLogger *_logger;
};
