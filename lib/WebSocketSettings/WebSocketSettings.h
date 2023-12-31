#pragma once
#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <EEPROM.h>

class WebSocketLogger;

class WebSocketSettings {
public:
  struct Scale {
    byte read_samples = 8;
    byte speed = 10;
    byte gain = 1;
    float calibration_factor = 0.0;
    float target_dose_single = 0.0;
    float target_dose_double = 0.0;
    float correction_dose_single = 0.0;
    float correction_dose_double = 0.0;

    bool is_changed = false;
  };

  struct WiFi {
    bool reset_flag = false;
  };

  WebSocketSettings();
  void begin(AsyncWebServer *srv, const WebSocketLogger *logger);
  void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data,
                        size_t len);

  Scale scale;
  WiFi wifi;

private:
  void loadScaleFromEEPROM();
  void saveScaleToEEPROM();

  void handleWebSocketText(const String &cmd, String &response);

  // EEPROM address to store the scale value
  static const int EEPROM_SCALE_ADDRESS;

  AsyncWebSocket _ws;
  AsyncWebServer *_server;
  const WebSocketLogger *_logger;
};
