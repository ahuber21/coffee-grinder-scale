#include <ADS1232.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

// needs to be included after WiFiManager.h
// which does not properly protect some defines
#include <API.h>
#include <Display.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketMetrics.h>
#include <WebSocketGraph.h>
#include <WebSocketLogger.h>
#include <WebSocketSettings.h>
#include <RawDataWebSocket.h>

#include "defines.h"

#define GRAMS_DIGITS 1
#define TIME_DIGITS 1

ADS1232 scale = ADS1232(ADC_PDWN_PIN, ADC_SCLK_PIN, ADC_DOUT_PIN, ADC_SPEED_PIN,
                        ADC_GAIN1_PIN, ADC_GAIN0_PIN);
Display display(DISPLAY_SCK_PIN, DISPLAY_MISO_PIN, DISPLAY_MOSI_PIN,
                DISPLAY_SS_PIN, DISPLAY_DC_PIN, DISPLAY_CS_PIN,
                DISPLAY_RESET_PIN, DISPLAY_BACKLIGHT_PIN);

AsyncWebServer server(SERVER_PORT);
API api;
WebSocketLogger logger;
WebSocketSettings settings;
WebSocketGraph graph;
WebSocketMetrics metrics;
RawDataWebSocket rawData;

// overall timeout when running the grinder
static const unsigned long timeout_millis = 30000;
// how long the final result is displayed
static const unsigned long finalize_screentime_millis = 8000;
// how long the confirm screen is shown
static const unsigned long confirm_timeout_millis = 2000;
// button debounce, accept one button press each X milliseconds
static const unsigned long button_debounce_millis = 150;
// minimum time to hold the button to be counted as true press (filter noise)
static const unsigned long button_debounce_min_hold = 20;
// stability detection timeouts
static const unsigned long stability_min_wait_millis = 500;
static const unsigned long stability_max_wait_millis = 1500;

bool grinder_is_running = false;

float target_grams = 0;
float target_grams_corrected = 0;  // includes the correction dose

// various millis to keep track of when stuff happened
unsigned long button_pressed_filter_millis = 0;
unsigned long button_pressed_millis = 0;
unsigned long debug_last_print_millis = 0;
unsigned long finalize_millis = 0;
unsigned long grinder_runtime_millis = 0;  // how long the grinder was on for
unsigned long grinder_started_millis = 0;  // when the grinder was started
unsigned long grinder_stopped_millis = 0;  // when the grinder was stopped
unsigned long last_grams_millis = 0;
unsigned long last_heartbeat_millis = 0;
unsigned long last_top_up_millis = 0;
unsigned long session_started_millis = 0;  // when the grind session was started
unsigned long state_change_to_idle_millis = 0;
unsigned long stability_wait_start_millis = 0;
unsigned long top_up_stop_millis = 0;

// various grams values to calculate differences between iterations
float grams_on_grinder_on = 0.0f;  // grams when grinder was turned on
float last_grams = 0.0f;
float top_up_grams_delta = 0.0f;

// to calculate the grams per second rate for the current run
float grams_per_seconds_total = 0;
uint16_t grams_per_seconds_count = 0;

// determined in finalize, displayed in stopping
float finalize_time = 0;
float finalize_grams = 0;
// ensure finalize events (graph + metrics) are only broadcast once
bool finalize_broadcast_done = false;

volatile enum State {
  IDLE = 0,
  BUTTON_FILTER,
  BUTTON_PRESSED,
  CONFIRM,
  TARE,
  CONFIGURED,
  RUNNING,
  TOPUP,
  STOPPING,
  FINALIZE,
  DEBUG,
} state;
// previous state when in loop
State old_state_loop = IDLE;
// previous state when button press interrupt is handled
State old_state_button_press = IDLE;

enum ButtonPin {
  none = 0,
  left = BUTTON_LEFT,
  right = BUTTON_RIGHT,
  back = BUTTON_BACK,
};

volatile ButtonPin button;       // the button that was currently pressed
volatile ButtonPin last_button;  // the button that was previously pressed

void setupDisplay();
void setupWifi();
void setupScale();

void loopIdle();
void loopButtonFilter();
void loopButtonPressed();
void loopButtonPressedDebug();
void loopConfirm();
void loopTare();
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

uint16_t getConnectionIndicatorColor();

template <ButtonPin pin>
void IRAM_ATTR button_interrupt() {
  auto now = millis();
  button_pressed_filter_millis = now;

  if ((now - button_pressed_millis) < button_debounce_millis) {
    return;
  }

  if (state == CONFIRM) {
    // go to configured if same button is pressed again
    // go to idle if the button is different
    state = (pin == last_button) ? TARE : IDLE;
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

  api.begin(server);
  logger.println("API ready");

  settings.begin(&server, &logger);
  logger.println("Settings ready");

  graph.begin(&server);
  logger.println("Graph ready");

  metrics.begin(&server, &logger);

  rawData.begin(server, "/RawDataWebSocket", 10.0f);
  logger.println("Raw data WebSocket ready");

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
  auto back_button_isr = []() IRAM_ATTR {
    old_state_button_press =
        (state == BUTTON_PRESSED) ? old_state_button_press : state;
    state = BUTTON_PRESSED;
    button = back;
  };
  attachInterrupt(digitalPinToInterrupt(BUTTON_BACK), back_button_isr, RISING);

  display.clear();
  state_change_to_idle_millis = millis();
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
    case TARE:
      loopTare();
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
      case TARE:
        message += "TARE";
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

  if (api.isNewValueReceived()) {
    float requested_grams = api.getNewValue();
    logger.println("API request for " + String(requested_grams, 2) + " g");
    target_grams = requested_grams;
    // correction hardcoded for now
    float correction = requested_grams > 1.5f ? 1.5f : 0.0f;
    target_grams_corrected = requested_grams - correction;
    // "virtually" press right button -> left cancel, right confirm
    last_button = right;
    button_pressed_millis = millis();
    state = CONFIRM;
    return;
  }

  float grams = scale.getUnits();
  if ((-0.3 < grams) && (grams < 0.3)) {
    grams = 0.0f;
  }
  display.displayIdleLayout(grams, getConnectionIndicatorColor());
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
      target_grams_corrected = settings.scale.target_dose_single -
                               settings.scale.top_up_margin_single;
      state = CONFIRM;
      break;
    case right:
      target_grams = settings.scale.target_dose_double;
      target_grams_corrected = settings.scale.target_dose_double -
                               settings.scale.top_up_margin_double;
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
  display.displayConfirmLayout(target_grams);

  if ((millis() - button_pressed_millis) > confirm_timeout_millis) {
    // go back to idle
    state_change_to_idle_millis = millis();
    state = IDLE;
    display.refresh();
  }
}

void loopTare() {
  // we tare the scale until we get a stable reading
  display.displayString("T", VerticalAlignment::CENTER);
  bool success = scale.tare();
  if (success) {
    // averages over ring buffer were stable, move on
    state = CONFIGURED;
  }
}

void loopConfigured() {
  // reset graph & metrics target
  graph.resetGraph(target_grams);
  graph.updateGraphData(0.0f, 0.0f);
  metrics.sendTarget(target_grams);

  display.displayString("T", VerticalAlignment::CENTER);

  // start grinder
  grinderOn();
  session_started_millis = millis();

  // update display
  display.clear();
  display.displayString(String(target_grams, GRAMS_DIGITS) + " g",
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

  if (now - session_started_millis > timeout_millis) {
    // timeout - no top up
    state = STOPPING;
    return;
  }

  float grams = scale.getUnits();

  float time = (now - session_started_millis) / 1000.;

  // update graph + metrics
  graph.updateGraphData(time, grams);
  metrics.sendProgress(time, grams);

  // log raw ADC data during grinding
  bool isStable;
  int32_t rawValue = scale.getRaw(isStable);
  rawData.sendRawData(rawValue, grams, now - session_started_millis, isStable);

  display.displayGrindingLayout(grams, target_grams, time, ST7735_WHITE, ST7735_WHITE, ST7735_WHITE, getConnectionIndicatorColor());

  // wait until something is happening
  if (grams < 1) {
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

  // continue to next state if we're consistently above target
  if (grams > last_grams && (last_grams > target_grams_corrected)) {
    state = TOPUP;
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

  // Display is handled by displayGrindingLayout above
}

void loopTopUp() {
  // stop the grinder
  // wait to stabilize
  // top up if necessary, based on weight calculation
  auto now = millis();
  float grams = scale.getUnits();
  float time = (now - session_started_millis) / 1000.;

  display.displayGrindingLayout(grams, target_grams, time,
                               ST7735_CYAN, ST7735_WHITE, ST7735_WHITE,
                               getConnectionIndicatorColor());

  graph.updateGraphData(time, grams);

  bool isStable;
  int32_t rawValue = scale.getRaw(isStable);
  rawData.sendRawData(rawValue, grams, now - session_started_millis, isStable);

  if (!grinder_is_running) {
    unsigned long wait_time = now - last_top_up_millis;

    float delta_grams = grams - grams_on_grinder_on;
    bool enough_weight = delta_grams >= settings.scale.min_topup_grams;
    bool enough_time = wait_time >= settings.scale.topup_timeout_ms;

    if (!enough_time) {
      if (!enough_weight || !isStable) {
        return;
      }
    }

    metrics.sendTopUp(grinder_runtime_millis, delta_grams);

    if (grams >= target_grams - 0.08) {
      logger.println("Target weight reached - stopping");
      // close enough to target weight
      state = STOPPING;
      return;
    }

    // calculate next top off time based on avg_rate
    float avg_rate = (grams_per_seconds_count > 0)
                         ? grams_per_seconds_total / grams_per_seconds_count
                         : 0.1;
    if (!(avg_rate > 0)) {
      logger.println("Zero avg_rate?");
      state = STOPPING;
      return;
    }
    // calculate how long we should run, only allowing a window of values
    float top_up_seconds = (target_grams - grams) / avg_rate;
    top_up_seconds = top_up_seconds < 0.5f ? 0.5f : top_up_seconds;
    top_up_seconds = top_up_seconds > 1.3f ? 1.3f : top_up_seconds;
    top_up_stop_millis = now + 1000. * top_up_seconds;
    logger.println("Top up for " + String(top_up_seconds, TIME_DIGITS) + " s");
    grinderOn();
  } else if (grinder_is_running && (now > top_up_stop_millis)) {
    grinderOff();
    logger.println("Top up done - waiting for settle");
    last_top_up_millis = now;
  }
}

void loopStopping() {
  if (grinder_is_running) {
    grinderOff();
    stability_wait_start_millis = millis();
    return;
  }

  auto now = millis();
  float grams = scale.getUnits();
  float time = (now - session_started_millis) / 1000.;

  display.displayGrindingLayout(grams, target_grams, time,
                               ST7735_CYAN, ST7735_WHITE, ST7735_WHITE,
                               getConnectionIndicatorColor());

  bool isStable;
  int32_t rawValue = scale.getRaw(isStable);
  rawData.sendRawData(rawValue, grams, now - session_started_millis, isStable);

  unsigned long wait_time = now - stability_wait_start_millis;

  // Check for stability or enforce maximum wait time
  if (!isStable && wait_time < stability_max_wait_millis) {
    logger.println("Waiting to stabilize");
    return;
  }

  char buffer[80];
  sprintf(buffer,
          "DONE | TIME %5.2f s | TOTAL WEIGHT %5.2f g | TARGET WEIGHT %5.2f",
          time, grams, target_grams);
  logger.println(buffer);

  time = (now - session_started_millis) / 1000.;
  graph.updateGraphData(time, grams);

  finalize_millis = millis();
  finalize_grams = grams;
  finalize_time = time;
  finalize_broadcast_done = false;
  state = FINALIZE;
}

void loopFinalize() {
  display.displayGrindingLayout(finalize_grams, target_grams, finalize_time,
                               ST7735_GREEN, ST7735_WHITE, ST7735_WHITE,
                               getConnectionIndicatorColor());
  if (!finalize_broadcast_done) {
    // send finalize events only once to avoid flooding websockets / heap

    // Send final raw data point
    bool isStable;
    int32_t rawValue = scale.getRaw(isStable);
    unsigned long timestamp = millis() - session_started_millis;
    rawData.sendRawData(rawValue, finalize_grams, timestamp, isStable);

    // Send completion event
    rawData.sendComplete();

    // Send other finalize events
    graph.finalizeGraph();
    metrics.sendFinalize(finalize_time, finalize_grams);
    finalize_broadcast_done = true;
  }

  if (millis() - finalize_millis > finalize_screentime_millis) {
    display.clear();
    state_change_to_idle_millis = millis();
    state = IDLE;
  }
}

void loopDebug() {
  IPAddress ip = WiFi.localIP();
  display.displayString(ip.toString(), VerticalAlignment::TWO_ROW_TOP);

  bool isStable;
  auto raw = scale.getRaw(isStable);

  if (millis() - debug_last_print_millis > 1000) {
    char logger_buffer[100];
    sprintf(logger_buffer, "Cal: %f - Raw: %ld %s - Grams: %f", settings.scale.calibration_factor, raw, isStable ? "S" : "P", scale.getUnits());
    logger.println(logger_buffer);
    char buffer[12];
    sprintf(buffer, "%d %s", raw, isStable ? "S" : "P");
    display.displayString(buffer,
                          VerticalAlignment::TWO_ROW_BOTTOM);
    debug_last_print_millis = millis();
  }
}

void grinderOn() {
  grams_on_grinder_on = scale.getUnits();
  digitalWrite(GRINDER_RELAY_PIN, HIGH);
  logger.println("Grinder started");
  grinder_started_millis = millis();
  grinder_is_running = true;
}
void grinderOff() {
  digitalWrite(GRINDER_RELAY_PIN, LOW);
  logger.println("Grinder stopped");
  grinder_stopped_millis = millis();
  grinder_runtime_millis = grinder_stopped_millis - grinder_started_millis;
  grinder_is_running = false;
}

void setupDisplay() {
  display.begin();
  display.wakeUp();
  display.setRotation(0);
  display.clear();
  display.displayString("Eureka", VerticalAlignment::CENTER);
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

uint16_t getConnectionIndicatorColor() {
  bool hasMetrics = metrics.getClientCount() > 0;
  bool hasRawData = rawData.getClientCount() > 0;

  if (hasMetrics && hasRawData) {
    return ST7735_GREEN;  // Both connected
  } else if (hasMetrics) {
    return ST7735_YELLOW; // Only metrics (old logger) connected
  } else if (hasRawData) {
    return ST7735_BLUE;   // Only raw data connected
  } else {
    return 0;             // No connections - no indicator
  }
}
