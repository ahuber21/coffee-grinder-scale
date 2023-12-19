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

bool state_on_interrupt_was_debug = false;
float target_grams = 0;
unsigned long grinder_started_millis = 0;
unsigned long last_heartbeat_millis = 0;
int32_t tare_raw = 0;

enum State {
  IDLE = 0,
  BUTTON_PRESSED,
  CONFIGURED,
  RUNNING,
  TIMEOUT,
  STOPPING,
  DEBUG,
} state;

enum ButtonPins {
  left = BUTTON_LEFT,
  right = BUTTON_RIGHT,
  back = BUTTON_BACK,
} button;

void setupScale();

void heartbeat();
void loopIdle();
void loopButtonPressed();
void loopButtonPressedDebug();
void loopConfigured();
void loopRunning();
void loopTimeout();
void loopStopping();
void loopDebug();

float units();

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
  setupScale();
  logger.println("Scale ready");

  pinMode(BUTTON_LEFT, INPUT_PULLDOWN);
  attachInterrupt(
      digitalPinToInterrupt(BUTTON_LEFT),
      []() {
        state_on_interrupt_was_debug = state == DEBUG;
        state = BUTTON_PRESSED;
        button = left;
      },
      RISING);
  pinMode(BUTTON_RIGHT, INPUT_PULLDOWN);
  attachInterrupt(
      digitalPinToInterrupt(BUTTON_RIGHT),
      []() {
        state_on_interrupt_was_debug = state == DEBUG;
        state = BUTTON_PRESSED;
        button = right;
      },
      RISING);
  pinMode(BUTTON_BACK, INPUT_PULLDOWN);
  attachInterrupt(
      digitalPinToInterrupt(BUTTON_BACK),
      []() {
        state_on_interrupt_was_debug = state == DEBUG;
        state = BUTTON_PRESSED;
        button = back;
      },
      RISING);

  state = IDLE;
}

void setupScale() {
  scale.setSmoothing(false);
  scale.setSpeed(settings.scale.speed);
  scale.setGain(settings.scale.gain);
  scale.setCalFactor(settings.scale.calibration_factor);
  tare_raw = scale.readRaw(settings.scale.read_samples);

  settings.scale.is_changed = false;
}

void loop() {
  heartbeat();

  switch (state) {
  case IDLE:
    loopIdle();
    break;
  case BUTTON_PRESSED:
    state_on_interrupt_was_debug ? loopButtonPressedDebug()
                                 : loopButtonPressed();
    break;
  case CONFIGURED:
    loopConfigured();
    break;
  case RUNNING:
    loopRunning();
    break;
  case TIMEOUT:
    loopTimeout();
    break;
  case STOPPING:
    loopStopping();
    break;
  case DEBUG:
    loopDebug();
    break;
  default:
    break;
  }
}

void heartbeat() {
  if (millis() - last_heartbeat_millis > 1000) {
    String message = "[heartbeat] state=";
    switch (state) {
    case IDLE:
      message += "IDLE";
      break;
    case BUTTON_PRESSED:
      message += "BUTTON_PRESSED";
      break;
    case CONFIGURED:
      message += "CONFIGURED";
      break;
    case RUNNING:
      message += "RUNNING";
      break;
    case TIMEOUT:
      message += "TIMEOUT";
      break;
    case STOPPING:
      message += "STOPPING";
      break;
    case DEBUG:
      message += "DEBUG";
      break;
    default:
      message += "UNHANDLED STATE: " + String((int)state);
      break;
    }
    logger.println(message);
    last_heartbeat_millis = millis();
  }
}

void loopIdle() {
  ArduinoOTA.handle();

  if (settings.scale.is_changed) {
    setupScale();
  }
}

void loopButtonPressed() {
  switch (button) {
  case left:
    target_grams = settings.scale.target_dose_single;
    state = CONFIGURED;
    break;
  case right:
    target_grams = settings.scale.target_dose_double;
    state = CONFIGURED;
    break;
  case back:
    state = DEBUG;
    break;
  default:
    state = IDLE;
    break;
  }
}

void loopButtonPressedDebug() {
  switch (button) {
  case left:
    tare_raw = scale.readRaw(settings.scale.read_samples);
    state = DEBUG;
    break;
  case right:
    state = DEBUG;
    break;
  case back:
    state = IDLE;
    break;
  default:
    state = IDLE;
    break;
  }
}

void loopConfigured() {
  // show stuff on display

  // tare
  tare_raw = scale.readRaw(settings.scale.read_samples);

  // turn on grinder
  grinder_started_millis = millis();

  state = RUNNING;
}

void loopRunning() {
  float grams = scale.readUnits(settings.scale.read_samples);
  logger.println("Running ... " + String(grams));

  // update display

  if (millis() - grinder_started_millis > 5000) {
    logger.println("Timeout");
    state = TIMEOUT;
  }
  if (grams >= target_grams) {
    state = STOPPING;
  }
}

void loopTimeout() {
  float grams = scale.readUnits(settings.scale.read_samples);

  // stop grinder

  // update display with timeout message

  state = IDLE;
}

void loopStopping() {
  float grams = scale.readUnits(settings.scale.read_samples);

  // stop grinder

  delay(100);
  for (int i = 0; i < 10; ++i) {
    float tmp = scale.readUnits(settings.scale.read_samples);
    if (tmp - grams < 0.1) {
      break;
    }
    delay(50);
  }

  // final display update

  state = IDLE;
}

void loopDebug() {
  auto raw = scale.readRaw(settings.scale.read_samples);
  logger.println("Cal: " + String(settings.scale.calibration_factor, 10));
  logger.println("Tare: " + String(tare_raw));
  logger.println("Raw: " + String(raw));
  logger.println("Grams: " + String(units()));
}

float units() {
  float raw = scale.readRaw(settings.scale.read_samples) - tare_raw;
  return raw * settings.scale.calibration_factor;
}