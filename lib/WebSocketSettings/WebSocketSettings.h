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
    float calibration_factor = 0.0f;
    float target_dose_single = 0.0f;
    float target_dose_double = 0.0f;
    float top_up_margin_single = 0.0f;
    float top_up_margin_double = 0.0f;
    float min_topup_grams = 0.08f;
    float rate_calculation_percentage = 0.75f;
    float rate_min_valid = 0.5f;
    float rate_max_valid = 2.0f;
    float rate_default = 1.0f;
    unsigned long topup_timeout_ms = 1000;
    unsigned long grinding_timeout_ms = 30000;
    unsigned long finalize_timeout_ms = 8000;
    unsigned long confirm_timeout_ms = 2000;
    unsigned long stability_min_wait_ms = 500;
    unsigned long stability_max_wait_ms = 1500;
    unsigned long button_debounce_ms = 150;
    unsigned long min_topup_runtime_ms = 500;
    unsigned long min_topup_interval_ms = 1000;
    unsigned long screensaver_timeout_s = 60;

    time_t last_coffee_timestamp = 0;

    uint32_t magic = 0xCAFEBABE;

    bool is_changed = false;
  };

  struct WiFi {
    bool reset_flag = false;
    bool reboot_flag = false;
  };

  WebSocketSettings();
  void begin(AsyncWebServer *srv, const WebSocketLogger *logger);
  void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data,
                        size_t len);

  Scale scale;
  WiFi wifi;

  void saveScaleToEEPROM();

 private:
  void loadScaleFromEEPROM();

  void handleWebSocketText(const String &cmd, String &response);

  // EEPROM address to store the scale value
  static const int EEPROM_SCALE_ADDRESS;

  AsyncWebSocket _ws;
  AsyncWebServer *_server;
  const WebSocketLogger *_logger;
};
