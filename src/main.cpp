#include <ADS1232.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

// needs to be included after WiFiManager.h
// which does not properly protect some defines
#include <Display.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketGraph.h>
#include <WebSocketLogger.h>
#include <WebSocketSettings.h>

#include "defines.h"

ADS1232 scale = ADS1232(ADC_PDWN_PIN, ADC_SCLK_PIN, ADC_DOUT_PIN, ADC_SPEED_PIN,
                        ADC_GAIN1_PIN, ADC_GAIN0_PIN);
Display display(DISPLAY_SCK_PIN, DISPLAY_MISO_PIN, DISPLAY_MOSI_PIN,
                DISPLAY_SS_PIN, DISPLAY_DC_PIN, DISPLAY_CS_PIN,
                DISPLAY_RESET_PIN, DISPLAY_BACKLIGHT_PIN);

AsyncWebServer server(SERVER_PORT);
WebSocketLogger logger;
WebSocketSettings settings;
WebSocketGraph graph;

// overall timeout when running the grinder
static const unsigned long timeout_millis = 30000;
// how long the final result is displayed
static const unsigned long finalize_screentime_millis = 2000;
// how long the confirm screen is shown
static const unsigned long confirm_timeout_millis = 2000;
// button debounce, accept one button press each X milliseconds
static const unsigned long button_debounce_millis = 150;
// minimum time to hold the button to be counted as true press (filter noise)
static const unsigned long button_debounce_min_hold = 20;

bool grinder_is_running = false;

float target_grams = 0;
float target_grams_corrected = 0; // includes the correction dose

// various millis to keep track of when stuff happened
unsigned long grinder_started_millis = 0;
unsigned long last_heartbeat_millis = 0;
unsigned long button_pressed_filter_millis = 0;
unsigned long button_pressed_millis = 0;
unsigned long last_grams_millis = 0;
unsigned long last_top_up_millis = 0;
unsigned long top_up_stop_millis = 0;
unsigned long stopping_last_millis = 0;
unsigned long finalize_millis = 0;

// various grams values to calculate differences between iterations
float last_grams = 0;
float stopping_last_grams = 0;

// to calculate the grams per second rate for the current run
float grams_per_seconds_total = 0;
uint16_t grams_per_seconds_count = 0;

// determined in finalize, displayed in stopping
float finalize_time = 0;
float finalize_grams = 0;

volatile enum State {
  IDLE = 0,
  BUTTON_FILTER,
  BUTTON_PRESSED,
  CONFIRM,
  CONFIGURED,
  RUNNING,
  TOPUP,
  STOPPING,
  FINALIZE,
  DEBUG,
} state;
State old_state_loop = IDLE;
State old_state_button_press = IDLE;

enum ButtonPin {
  none = 0,
  left = BUTTON_LEFT,
  right = BUTTON_RIGHT,
  back = BUTTON_BACK,
};

volatile ButtonPin button;      // the button that was currently pressed
volatile ButtonPin last_button; // the button that was previously pressed

void setupDisplay();
void setupWifi();
void setupScale();

void loopIdle();
void loopButtonFilter();
void loopButtonPressed();
void loopButtonPressedDebug();
void loopConfirm();
void loopConfigured();
void loopRunning();
void loopTopUp();
void loopStopping();
void loopFinalize();
void loopDebug();

void resetWifi();

void heartbeat();

void grinderOn();
void grinderOff();

template <ButtonPin pin> void button_interrupt() {
  auto now = millis();
  button_pressed_filter_millis = now;

  if ((now - button_pressed_millis) < button_debounce_millis) {
    return;
  }

  if (state == CONFIRM) {
    // go to configured if same button is pressed again
    // go to idle if the button is different
    state = (pin == last_button) ? CONFIGURED : IDLE;
  } else if (state == IDLE) {
    old_state_button_press = IDLE;
    state = BUTTON_FILTER;
    button = pin;
  }

  // reset the last_button to avoid auto-confirm / -cancel in the next run
  last_button = none;
}

void setup() {
  Serial.begin(115200);

  // grinder relay
  pinMode(GRINDER_RELAY_PIN, OUTPUT);
  grinderOff();

  // display
  setupDisplay();

  setupWifi();

  server.begin();
  logger.begin(&server);
  logger.println("Logger ready");

  settings.begin(&server, &logger);
  logger.println("Settings ready");

  graph.begin(&server);
  logger.println("Graph ready");

  ArduinoOTA.begin();
  ArduinoOTA.onStart([]() { display.clear(); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char buffer[10];
    float percentage = progress / (total / 100.0f);
    sprintf(buffer, "%3.0f %%", percentage);

    logger.println(buffer);
    display.displayString("UPDATE", VerticalAlignment::TWO_ROW_TOP);
    display.displayString(buffer, VerticalAlignment::TWO_ROW_BOTTOM);
  });

  // power up the scale circuit
  pinMode(ADC_LDO_EN_PIN, OUTPUT);
  digitalWrite(ADC_LDO_EN_PIN, HIGH);
  setupScale();
  logger.println("Scale ready");

  pinMode(BUTTON_LEFT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT), button_interrupt<left>,
                  FALLING);
  pinMode(BUTTON_RIGHT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT), button_interrupt<right>,
                  FALLING);
  pinMode(BUTTON_BACK, INPUT_PULLDOWN);
  attachInterrupt(
      digitalPinToInterrupt(BUTTON_BACK),
      []() {
        old_state_button_press =
            (state == BUTTON_PRESSED) ? old_state_button_press : state;
        state = BUTTON_PRESSED;
        button = back;
      },
      RISING);

  display.clear();
  state = IDLE;
}

void setupScale() {
  if (!scale.begin()) {
    logger.println("scale.begin() error");
    ESP.restart();
  };
  scale.setSpeed(settings.scale.speed);
  scale.setGain(settings.scale.gain);
  scale.setCalFactor(settings.scale.calibration_factor);
  scale.setRingBufferSize(settings.scale.read_samples);
  scale.initRingBuffer();
  scale.tare();

  settings.scale.is_changed = false;
}

void loop() {
  heartbeat();

  // read the ADC if it's ready - this is close to non-blocking
  scale.readADCIfReady();

  // don't call expensive functions between check and setting old_state_loop
  // as new interrupts may occur in the meantime
  bool need_to_clear = state != old_state_loop;
  old_state_loop = state;
  if (need_to_clear) {
    display.clear();
  }

  switch (state) {
  case IDLE:
    loopIdle();
    break;
  case BUTTON_FILTER:
    loopButtonFilter();
    break;
  case BUTTON_PRESSED:
    if (old_state_button_press == DEBUG) {
      loopButtonPressedDebug();
    } else if (old_state_button_press == IDLE) {
      loopButtonPressed();
    } else {
      // ignore button press
      state = old_state_button_press;
    }
    break;
  case CONFIRM:
    loopConfirm();
    break;
  case CONFIGURED:
    loopConfigured();
    break;
  case RUNNING:
    loopRunning();
    break;
  case TOPUP:
    loopTopUp();
    break;
  case STOPPING:
    loopStopping();
    break;
  case FINALIZE:
    loopFinalize();
    break;
  case DEBUG:
    loopDebug();
    break;
  default:
    break;
  }
}

void heartbeat() {
  if (millis() - last_heartbeat_millis > 5000) {
    String message = "[heartbeat] state=";
    switch (state) {
    case IDLE:
      message += "IDLE";
      break;
    case BUTTON_FILTER:
      message += "BUTTON_FILTER";
      break;
    case BUTTON_PRESSED:
      message += "BUTTON_PRESSED";
      break;
    case CONFIGURED:
      message += "CONFIGURED";
      break;
    case CONFIRM:
      message += "CONFIRM";
      break;
    case RUNNING:
      message += "RUNNING";
      break;
    case TOPUP:
      message += "TOPUP";
      break;
    case STOPPING:
      message += "STOPPING";
      break;
    case FINALIZE:
      message += "FINALIZE";
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

  if (settings.wifi.reset_flag) {
    resetWifi();
  }

  float grams = scale.getUnits();
  display.displayString(String(grams, 2) + " g", VerticalAlignment::CENTER);
}

void loopButtonFilter() {
  auto now = millis();

  if (now - button_pressed_filter_millis > button_debounce_min_hold) {
    // we held the button long enough, it's probably not noise, can move on
    state = BUTTON_PRESSED;
    return;
  }

  // read the state of the pressed button
  int button_state = digitalRead(button);

  // interrupt is falling, check if button is still in correct state
  if (button_state != LOW) {
    // just noise, ignore
    state = IDLE;
  }
}

void loopButtonPressed() {
  last_button = button;
  button_pressed_millis = millis();

  switch (button) {
  case left:
    target_grams = settings.scale.target_dose_single;
    target_grams_corrected =
        settings.scale.target_dose_single - settings.scale.top_up_margin_single;
    state = CONFIRM;
    break;
  case right:
    target_grams = settings.scale.target_dose_double;
    target_grams_corrected =
        settings.scale.target_dose_double - settings.scale.top_up_margin_double;
    state = CONFIRM;
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

void loopConfirm() {
  display.displayString("CONFIRM", TWO_ROW_TOP);
  display.displayString(String(target_grams) + " g", TWO_ROW_BOTTOM);

  if ((millis() - button_pressed_millis) > confirm_timeout_millis) {
    // go back to idle
    state = IDLE;
  }
}

void loopConfigured() {
  // reset graph
  graph.resetGraph(target_grams);
  graph.updateGraphData(0.0f, 0.0f);

  display.displayString("T", VerticalAlignment::CENTER);

  // tare
  scale.tare();

  // start grinder
  grinderOn();
  logger.println("Grinder started");
  grinder_started_millis = millis();

  // update display
  display.clear();
  display.displayString(String(target_grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_BOTTOM);

  // initial values
  last_grams = scale.getUnits();
  last_grams_millis = millis();
  grams_per_seconds_total = 0;
  grams_per_seconds_count = 0;

  state = RUNNING;
}

void loopRunning() {
  unsigned long int now = millis();

  if (now - grinder_started_millis > timeout_millis) {
    // timeout - no top up
    state = STOPPING;
    return;
  }

  float grams = scale.getUnits();

  float time = (now - grinder_started_millis) / 1000.;

  // update graph
  graph.updateGraphData(time, grams);

  // update display top line (time)
  display.displayString("R - " + String(time, 2) + " s",
                        VerticalAlignment::THREE_ROW_TOP);

  // wait until something is happening
  if (grams < 1) {
    return;
  }

  // update display grams
  display.displayString(String(grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_CENTER);

  if (grams > target_grams_corrected) {
    state = TOPUP;
    return;
  }

  // calculate weight increase
  float delta_grams = grams - last_grams;
  float delta_millis = now - last_grams_millis;
  if (delta_grams < 0.2) {
    return;
  }

  // calculate rate
  float rate = 1000. * delta_grams / delta_millis;
  if (rate < 0) {
    return;
  }

  // update last values with current ones
  last_grams = grams;
  last_grams_millis = now;

  // update the values for average calculation
  if (rate > 0.1) {
    grams_per_seconds_total += rate;
    ++grams_per_seconds_count;
  }

  // calculate average rate
  float avg_rate = (grams_per_seconds_count > 0)
                       ? grams_per_seconds_total / grams_per_seconds_count
                       : 0;

  if (!(avg_rate > 0)) {
    return;
  }

  // send formatted log message
  char buffer[130];
  sprintf(buffer,
          "TIME %5.2f s | WEIGHT %+5.2f g | RATE %+3.2f g/s | RATE (AVG) "
          "%+3.2f g/s",
          time, grams, rate, avg_rate);
  logger.println(buffer);

  // update display bottom (always stays the same)
  display.displayString(String(target_grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_BOTTOM);
}

void loopTopUp() {
  // stop the grinder
  // wait to stabilize
  // top up if necessary, based on weight calculation
  auto now = millis();
  float grams = scale.getUnits();
  float time = (now - grinder_started_millis) / 1000.;
  display.displayString(String(time, 2) + " s",
                        VerticalAlignment::THREE_ROW_TOP);
  display.displayString(String(grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_CENTER);
  display.displayString("TOPUP", VerticalAlignment::THREE_ROW_BOTTOM);

  graph.updateGraphData(time, grams);

  if (grams >= target_grams - 0.05) {
    logger.println("Target weight reached - stopping");
    // close enough to target weight
    state = STOPPING;
    return;
  }

  if (!grinder_is_running &&
      ((now - last_top_up_millis) > settings.scale.settle_millis)) {
    // grinder was turned off
    float avg_rate = (grams_per_seconds_count > 0)
                         ? grams_per_seconds_total / grams_per_seconds_count
                         : 0.1;
    if (!(avg_rate > 0)) {
      logger.println("Zero avg_rate?");
      state = STOPPING;
      return;
    }
    float top_up_seconds = (target_grams - grams) / avg_rate;
    top_up_seconds = top_up_seconds < 0.4f ? 0.4f : top_up_seconds;
    top_up_seconds = top_up_seconds > 1.0f ? 1.0f : top_up_seconds;
    top_up_stop_millis = now + 1000. * top_up_seconds;
    logger.println("Top up for " + String(top_up_seconds, 2) + " s");
    grinderOn();
  } else if (grinder_is_running && (now > top_up_stop_millis)) {
    logger.println("Top up done - waiting for settle");
    grinderOff();
    last_top_up_millis = now;
  }
}

void loopStopping() {
  if (grinder_is_running) {
    // stop grinder
    grinderOff();
    logger.println("Grinder stopped");
  }

  float grams = scale.getUnits();

  // update display
  float time = (millis() - grinder_started_millis) / 1000.;
  display.displayString(String(time, 2) + " s",
                        VerticalAlignment::THREE_ROW_TOP);
  display.displayString(String(grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_CENTER);

  auto now = millis();

  if (now - stopping_last_millis < 200) {
    return;
  }

  stopping_last_millis = now;
  float delta_grams = abs(grams - stopping_last_grams);
  stopping_last_grams = grams;

  time = (now - grinder_started_millis) / 1000.;
  // graph update
  graph.updateGraphData(time, grams);

  if (delta_grams > 0.1) {
    logger.println("Waiting to stabilize");
    display.displayString("STABILIZING", VerticalAlignment::THREE_ROW_BOTTOM);
    return;
  }

  char buffer[80];
  sprintf(buffer,
          "DONE | TIME %5.2f s | TOTAL WEIGHT %5.2f g | TARGET WEIGHT %5.2f",
          time, grams, target_grams);
  logger.println(buffer);

  finalize_millis = millis();
  finalize_grams = grams;
  finalize_time = time;
  state = FINALIZE;
}

void loopFinalize() {
  display.displayString(String(finalize_time, 2) + " s",
                        VerticalAlignment::THREE_ROW_TOP);
  display.displayString(String(finalize_grams, 2) + " g",
                        VerticalAlignment::THREE_ROW_CENTER);
  display.displayString("FINAL", VerticalAlignment::THREE_ROW_BOTTOM);

  graph.finalizeGraph();

  if (millis() - finalize_millis > finalize_screentime_millis) {
    display.clear();
    state = IDLE;
  }
}

void loopDebug() {
  IPAddress ip = WiFi.localIP();
  display.displayString(ip.toString(), VerticalAlignment::CENTER);

  auto raw = scale.getRaw();
  logger.println("Cal: " + String(settings.scale.calibration_factor, 10));
  logger.println("Raw: " + String(raw));
  logger.println("Grams: " + String(scale.getUnits()));
}

void grinderOn() {
  digitalWrite(GRINDER_RELAY_PIN, HIGH);
  grinder_is_running = true;
}
void grinderOff() {
  digitalWrite(GRINDER_RELAY_PIN, LOW);
  grinder_is_running = false;
}

void setupDisplay() {
  display.begin();
  display.wakeUp();
  display.setRotation(3);
  display.setBrightness(1);
  display.clear();
  display.drawBitmap(0, 0, (unsigned char *)bootLogo);
}

void setupWifi() {
  WiFiManager wifiManager;

  bool wifiWebServerStarted = false;

  wifiManager.setWebServerCallback(
      [&wifiWebServerStarted]() { wifiWebServerStarted = true; });
  wifiManager.setAPCallback([](WiFiManager *manager) {
    display.clear();
    display.displayString("AP started", VerticalAlignment::CENTER);
  });
  wifiManager.setSaveConfigCallback([]() {
    display.clear();
    display.displayString("Saving WiFi &", VerticalAlignment::TWO_ROW_TOP);
    display.displayString("rebooting", VerticalAlignment::TWO_ROW_BOTTOM);
  });

  wifiManager.autoConnect("Eureka setup");

  if (wifiWebServerStarted) {
    ESP.restart();
  }
}

void resetWifi() {
  WiFiManager manager;
  manager.resetSettings();

  display.clear();
  display.displayString("WiFi reset", VerticalAlignment::CENTER);

  logger.println("WiFi reset");

  settings.wifi.reset_flag = false;

  ESP.restart();
}
