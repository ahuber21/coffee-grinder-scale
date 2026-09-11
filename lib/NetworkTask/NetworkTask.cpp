#include "NetworkTask.h"

#include <Arduino.h>
#include <WiFi.h>
// WiFiManager.h must come before ESPAsyncWebServer.h: it pulls in the
// synchronous WebServer.h, whose WEBSERVER_H include guard is what makes
// ESPAsyncWebServer.h skip redefining the HTTP_GET/POST/... enum (it
// `#ifndef WEBSERVER_H`-guards its own copy) -- reversed, the two libraries'
// enums conflict at compile time. Same ordering the pre-rewrite
// src/main.cpp used, for the same reason (see its comment there).
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"  // SERVER_PORT

bool otaSafeToStart() {
  // D12: refuse, don't abort. DOSING_ACTIVE is set/cleared by Dosing task on
  // entering/leaving anything other than IDLE/SCREENSAVER (see DosingTask.cpp).
  EventBits_t bits = xEventGroupGetBits(g_sys_events);
  return (bits & kDosingActiveBit) == 0;
}

namespace {

// WiFiManager AP name, reused verbatim from the pre-rewrite src/main.cpp's
// setupWifi() (git history: "Eureka setup") -- no reason found to change it.
constexpr const char *kApName = "Eureka setup";

// D5: mDNS hostname -> eureka.local, no OTA password (accepted risk, home
// LAN only -- see DECISIONS.md D5; settled, not revisited here).
constexpr const char *kMdnsHostname = "eureka";
constexpr uint16_t kOtaPort = 3232;

// AR-014 fix: one WS, one connection-cap policy, picked once, here.
constexpr size_t kMaxWsClients = 4;

AsyncWebServer g_server(SERVER_PORT);
AsyncWebSocket g_ws("/ws");

uint32_t g_next_dose_request_id = 1;

// ---------------------------------------------------------------------------
// D4 multiplexed WS envelope: {"type": <discriminator>, ...fields}.
// Replaces the five old separate sockets (WebSocketLogger, WebSocketGraph,
// WebSocketMetrics, RawDataWebSocket, WebSocketSettings) with one channel;
// each TelemetryType (Messages.h) maps to one envelope "type" string, plus
// "settings" for the SettingsSnapshot broadcast and "error" for a per-client
// rejection reply. Most of these types don't have a rendering consumer yet
// (no SPA) -- the scaffolding is what's real here, not a full frontend
// contract.
// ---------------------------------------------------------------------------

const char *telemetryTypeToString(TelemetryType t) {
  switch (t) {
    case TelemetryType::TARGET:
      return "target";
    case TelemetryType::PROGRESS:
      return "progress";
    case TelemetryType::RAW_SAMPLE:
      return "raw_sample";
    case TelemetryType::TOPUP_PULSE:
      return "topup_pulse";
    case TelemetryType::FINALIZE:
      return "finalize";
    case TelemetryType::COMPLETE:
      return "complete";
    case TelemetryType::LOG_LINE:
      return "log";
  }
  return "unknown";
}

String buildTelemetryJson(const TelemetryEvent &ev) {
  JsonDocument doc;
  doc["type"] = telemetryTypeToString(ev.type);
  doc["session_id"] = ev.session_id;
  doc["runtime_ms"] = ev.runtime_ms;
  switch (ev.type) {
    case TelemetryType::LOG_LINE:
      doc["line"] = ev.log_line;
      break;
    case TelemetryType::RAW_SAMPLE:
      doc["raw_adc"] = ev.raw_adc;
      doc["grams"] = ev.grams;
      doc["stable"] = ev.stable;
      break;
    case TelemetryType::TOPUP_PULSE:
      doc["grams"] = ev.grams;
      doc["delta_grams"] = ev.delta_grams;
      doc["target_grams"] = ev.target_grams;
      break;
    case TelemetryType::TARGET:
    case TelemetryType::PROGRESS:
    case TelemetryType::FINALIZE:
    case TelemetryType::COMPLETE:
      doc["grams"] = ev.grams;
      doc["target_grams"] = ev.target_grams;
      break;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String buildSettingsJson(const SettingsSnapshot &s) {
  JsonDocument doc;
  doc["type"] = "settings";
  doc["version"] = s.version;
  doc["target_dose_single"] = s.target_dose_single;
  doc["target_dose_double"] = s.target_dose_double;
  doc["top_up_margin_single"] = s.top_up_margin_single;
  doc["top_up_margin_double"] = s.top_up_margin_double;
  doc["min_topup_grams"] = s.min_topup_grams;
  doc["button_debounce_ms"] = s.button_debounce_ms;
  doc["screensaver_timeout_s"] = s.screensaver_timeout_s;
  String out;
  serializeJson(doc, out);
  return out;
}

void sendError(AsyncWebSocketClient *client, const char *message, uint32_t request_id) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["message"] = message;
  doc["request_id"] = request_id;
  String out;
  serializeJson(doc, out);
  client->text(out);
}

bool settingsFieldFromName(const char *name, SettingsFieldId &out) {
  if (strcmp(name, "calibration_factor") == 0) {
    out = SettingsFieldId::CALIBRATION_FACTOR;
    return true;
  }
  if (strcmp(name, "target_dose_single") == 0) {
    out = SettingsFieldId::TARGET_DOSE_SINGLE;
    return true;
  }
  if (strcmp(name, "target_dose_double") == 0) {
    out = SettingsFieldId::TARGET_DOSE_DOUBLE;
    return true;
  }
  if (strcmp(name, "top_up_margin_single") == 0) {
    out = SettingsFieldId::TOP_UP_MARGIN_SINGLE;
    return true;
  }
  if (strcmp(name, "top_up_margin_double") == 0) {
    out = SettingsFieldId::TOP_UP_MARGIN_DOUBLE;
    return true;
  }
  if (strcmp(name, "button_debounce_ms") == 0) {
    out = SettingsFieldId::BUTTON_DEBOUNCE_MS;
    return true;
  }
  if (strcmp(name, "wifi_reset_flag") == 0) {
    out = SettingsFieldId::WIFI_RESET_FLAG;
    return true;
  }
  if (strcmp(name, "wifi_reboot_flag") == 0) {
    out = SettingsFieldId::WIFI_REBOOT_FLAG;
    return true;
  }
  return false;
}

// §3.6/§4: Network task only does cheap syntactic validation and enqueues a
// request to the task that owns the state -- it never reaches into another
// task's memory, no matter how the eventual SPA's message shapes evolve.
void handleWsMessage(AsyncWebSocketClient *client, const uint8_t *data, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    sendError(client, "malformed JSON", 0);
    return;
  }

  const char *type = doc["type"] | "";
  uint32_t request_id = doc["request_id"] | 0;

  if (strcmp(type, "settings_write") == 0) {
    const char *fieldName = doc["field"] | "";
    SettingsFieldId fieldId;
    if (!settingsFieldFromName(fieldName, fieldId)) {
      sendError(client, "unknown settings field", request_id);
      return;
    }
    SettingsWriteRequest req{};
    req.field_id = fieldId;
    req.request_id = request_id;
    if (fieldId == SettingsFieldId::BUTTON_DEBOUNCE_MS) {
      req.value.u = doc["value"] | static_cast<uint32_t>(0);
    } else if (fieldId == SettingsFieldId::WIFI_RESET_FLAG ||
               fieldId == SettingsFieldId::WIFI_REBOOT_FLAG) {
      req.value.b = doc["value"] | false;
    } else {
      req.value.f = doc["value"] | NAN;
    }
    // Settings task is the sole authority on whether this value is actually
    // legal (AR-016) -- this is only a syntactic "does this look like a
    // request" check, per §0.6/§4.
    if (xQueueSend(g_settings_write_q, &req, 0) != pdTRUE) {
      sendError(client, "settings queue full, try again", request_id);
    }
    return;
  }

  if (strcmp(type, "dose_request") == 0) {
    float grams = doc["grams"] | 0.0f;
    // Same range check as Dosing task's own defense-in-depth check (§3.6,
    // DosingTask.cpp) -- the fast-fail half of the AR-015 fix; Dosing still
    // re-validates itself, this doesn't replace that.
    if (!std::isfinite(grams) || grams <= 0.0f || grams > 40.0f) {
      sendError(client, "grams out of range (0, 40]", request_id);
      return;
    }
    DoseRequest req{grams, request_id != 0 ? request_id : g_next_dose_request_id++};
    if (xQueueSend(g_dose_request_q, &req, 0) != pdTRUE) {
      sendError(client, "dose queue full, try again", request_id);
    }
    return;
  }

  sendError(client, "unknown message type", request_id);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      Serial.printf("[Network] WS client #%u connected from %s\n", client->id(),
                    client->remoteIP().toString().c_str());
      if (server->count() > kMaxWsClients) {
        // AR-014 fix: one cap, enforced in the one place a client can join
        // (there's only one socket now, so there's only one place to pick).
        client->close();
        return;
      }
      // New clients get the current settings snapshot immediately rather
      // than waiting for the next write -- useful once a settings UI exists.
      SettingsSnapshot snap;
      if (xQueuePeek(g_settings_mailbox_network, &snap, 0) == pdTRUE) {
        client->text(buildSettingsJson(snap));
      }
      break;
    }
    case WS_EVT_DISCONNECT:
      Serial.printf("[Network] WS client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        handleWsMessage(client, data, len);
      }
      // Fragmented/binary frames aren't supported by this scaffolding yet --
      // control messages are small enough not to need it.
      break;
    }
    case WS_EVT_ERROR:
    case WS_EVT_PONG:
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// OTA (§7, D12: refuse outright, never abort an active grind).
//
// ArduinoOTA's public callback surface doesn't give a way to refuse *before*
// Update.begin() runs: looking at the vendored library source
// (framework-arduinoespressif32/libraries/ArduinoOTA/src/ArduinoOTA.cpp),
// handle() only calls onStart() (via _runUpdate()) *after* Update.begin()
// has already erased the target partition. The actual "refuse before it
// starts" gate is therefore in networkTaskFn's loop below: ArduinoOTA.handle()
// is simply never pumped while otaSafeToStart() is false, so the initial
// UDP handshake that would move the library into OTA_RUNUPDATE never
// happens -- the espota client just doesn't get a response until Dosing task
// leaves GRINDING/TOPUP/etc, at which point a flash proceeds completely
// normally. No grind is ever interrupted (D12's actual promise). onStart()
// re-checks and Update.abort()s as a narrow defense-in-depth backstop for
// the handful-of-ms race between a handshake completing and the next
// handle() call actually invoking _runUpdate() -- if that race is lost, the
// worst case is a wasted partition erase, never a disturbed grind.
// ---------------------------------------------------------------------------

void handleOtaStart() {
  if (!otaSafeToStart()) {
    Serial.println("[Network] OTA refused: grind in progress (D12)");
    Update.abort();
    return;
  }
  xEventGroupSetBits(g_sys_events, kOtaInProgressBit);
  Serial.println("[Network] OTA starting");
  DisplayCommand cmd{};
  cmd.mode = DisplayMode::OTA_UPDATE;
  cmd.ota_percent = 0;
  // §7's one deliberate second writer to the display mailbox -- Dosing task
  // never assigns OTA_UPDATE, and this only fires during the narrow,
  // rare OTA-flash window signaled by kOtaInProgressBit.
  xQueueOverwrite(g_display_mailbox, &cmd);
}

void handleOtaEnd() {
  xEventGroupClearBits(g_sys_events, kOtaInProgressBit);
  Serial.println("[Network] OTA finished, rebooting");
}

void handleOtaProgress(unsigned int progress, unsigned int total) {
  DisplayCommand cmd{};
  cmd.mode = DisplayMode::OTA_UPDATE;
  cmd.ota_percent = total ? static_cast<uint8_t>((progress * 100u) / total) : 0;
  xQueueOverwrite(g_display_mailbox, &cmd);
}

void handleOtaError(ota_error_t error) {
  xEventGroupClearBits(g_sys_events, kOtaInProgressBit);
  Serial.printf("[Network] OTA error [%u]\n", static_cast<unsigned>(error));
}

// ---------------------------------------------------------------------------
// WiFi provisioning -- config/AP-name pattern reused from the pre-rewrite
// src/main.cpp's setupWifi()/resetWifi() (git history, "6576a6d blank
// project with wifi manager & ota" onward): WiFiManager, AP name
// "Eureka setup", no config-portal timeout (blocks until configured, same
// as before), reboot after the portal saves new credentials. Not reused:
// the old code drove the ST7735 display directly from these callbacks
// (display.displayString("AP started", ...)) -- Display task now
// exclusively owns the display (§2), and Network task isn't the one
// deliberate second writer to DisplayCommand (that's OTA_UPDATE only, §7),
// so portal-active/saving status is logged, not shown on screen. This is a
// real (small) behavior change from the old firmware, worth a STATUS.md/AR
// note -- see the task report.
// ---------------------------------------------------------------------------

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kMdnsHostname);

  WiFiManager wm;
  bool portalWebServerStarted = false;
  wm.setWebServerCallback([&portalWebServerStarted]() { portalWebServerStarted = true; });
  wm.setAPCallback([](WiFiManager *) {
    Serial.println("[Network] WiFi config portal active: 'Eureka setup'");
  });
  wm.setSaveConfigCallback([]() { Serial.println("[Network] WiFi credentials saved"); });

  // Blocks here until WiFi is connected or the portal saves new credentials
  // -- matches the old code's untimed autoConnect(). Only Network task
  // blocks; every other task is already running independently (§8:
  // WIFI_CONNECTED is deliberately not part of the boot gate).
  wm.autoConnect(kApName);

  if (portalWebServerStarted) {
    // Mirrors the old code: start clean rather than continuing in the same
    // runtime once new credentials were saved via the portal.
    delay(100);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    xEventGroupSetBits(g_sys_events, kWifiConnectedBit);
    Serial.printf("[Network] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[Network] WiFi not connected; continuing offline");
  }
}

void setupMdnsAndOta() {
  if (!MDNS.begin(kMdnsHostname)) {
    Serial.println("[Network] mDNS init failed");
  } else {
    MDNS.addService("http", "tcp", SERVER_PORT);
    Serial.printf("[Network] mDNS up: %s.local\n", kMdnsHostname);
  }

  ArduinoOTA.setHostname(kMdnsHostname);
  ArduinoOTA.setMdnsEnabled(false);  // we already own mDNS above; avoid a second MDNS.begin()
  ArduinoOTA.setPort(kOtaPort);
  // D5: no OTA password -- accepted risk, home LAN only.
  ArduinoOTA.onStart(handleOtaStart);
  ArduinoOTA.onEnd(handleOtaEnd);
  ArduinoOTA.onProgress(handleOtaProgress);
  ArduinoOTA.onError(handleOtaError);
  ArduinoOTA.begin();

  // Advertise the OTA service ourselves since ArduinoOTA's own mDNS
  // integration is disabled above.
  MDNS.addService("arduino", "tcp", kOtaPort);
}

// §3.6: the API endpoint isn't its own task (design doc §1) -- a thin
// handler living in Network task's AsyncWebServer, cheap syntactic
// validation only, forwards to Dosing task via g_dose_request_q. Fixes
// AR-015's two halves: range-checked input (matches Dosing's own
// defense-in-depth bound) instead of toFloat()'s silent-zero-on-garbage,
// and a real JSON serializer instead of the old string concatenation.
void handleGetDosage(AsyncWebServerRequest *request) {
  JsonDocument doc;
  String out;

  if (!request->hasParam("grams")) {
    doc["error"] = "missing parameter: grams";
    serializeJson(doc, out);
    request->send(400, "application/json", out);
    return;
  }

  float grams = request->getParam("grams")->value().toFloat();
  if (!std::isfinite(grams) || grams <= 0.0f || grams > 40.0f) {
    doc["error"] = "grams out of range (0, 40]";
    serializeJson(doc, out);
    request->send(400, "application/json", out);
    return;
  }

  DoseRequest req{grams, g_next_dose_request_id++};
  if (xQueueSend(g_dose_request_q, &req, 0) != pdTRUE) {
    doc["error"] = "device busy, try again";
    serializeJson(doc, out);
    request->send(503, "application/json", out);
    return;
  }

  doc["accepted_grams"] = grams;
  doc["request_id"] = req.request_id;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// D4 scope note: the SPA/LittleFS-served filesystem is separate, later work
// (explicitly out of scope for this pass) -- "/" is a placeholder so the
// server is genuinely reachable and self-describing at eureka.local in the
// meantime.
void setupWebServer() {
  g_ws.onEvent(onWsEvent);
  g_server.addHandler(&g_ws);

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html",
                  "<!doctype html><html><body>"
                  "<h1>Eureka</h1>"
                  "<p>SPA not deployed yet. Realtime channel: <code>/ws</code>. "
                  "Dose API: <code>/api/getDosage?grams=N</code>.</p>"
                  "</body></html>");
  });

  g_server.on("/api/getDosage", HTTP_GET, handleGetDosage);

  g_server.onNotFound(
      [](AsyncWebServerRequest *request) { request->send(404, "text/plain", "not found"); });

  g_server.begin();
  Serial.printf("[Network] AsyncWebServer started on port %u\n", SERVER_PORT);
}

void networkTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE, portMAX_DELAY);

  setupWifi();
  setupMdnsAndOta();
  setupWebServer();

  SettingsSnapshot snap;
  uint32_t lastSeenVersion = 0;

  for (;;) {
    // §4: re-broadcast the settings snapshot to WS clients on every version
    // change, and act on the WiFi reset/reboot flags Network task owns the
    // read side of (§2's ownership map).
    if (xQueuePeek(g_settings_mailbox_network, &snap, 0) == pdTRUE &&
        snap.version != lastSeenVersion) {
      lastSeenVersion = snap.version;
      g_ws.textAll(buildSettingsJson(snap));

      if (snap.wifi_reset_flag) {
        Serial.println("[Network] wifi_reset_flag set -- clearing WiFi credentials, rebooting");
        WiFiManager wm;
        wm.resetSettings();
        delay(100);
        ESP.restart();
      } else if (snap.wifi_reboot_flag) {
        Serial.println("[Network] wifi_reboot_flag set -- rebooting");
        delay(100);
        ESP.restart();
      }
    }

    // §7: drain Telemetry task's WS-broadcast inbox and fan each event out
    // as one multiplexed envelope (D4) to every connected client.
    TelemetryEvent ev;
    while (xQueueReceive(g_ws_broadcast_q, &ev, 0) == pdTRUE) {
      g_ws.textAll(buildTelemetryJson(ev));
    }

    // AR-013 fix: something now actually calls this, once per tick.
    g_ws.cleanupClients();

    // D12: only pump ArduinoOTA's protocol handling while it's safe to --
    // see the OTA section above for why this (not onStart alone) is the
    // real "refuse before it starts" gate.
    if (otaSafeToStart()) {
      ArduinoOTA.handle();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void createNetworkTask() {
  xTaskCreatePinnedToCore(networkTaskFn, "Network", TaskConfig::kNetworkStackBytes, nullptr,
                           TaskConfig::kNetworkPriority, nullptr, TaskConfig::kNetworkCore);
}
