
#include "WebSocketSettings.h"

#include "ArduinoJson.h"
#include "WebSocketLogger.h"
#include <ESPAsyncWebServer.h>

namespace websettings {
const char PROGMEM config_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Config Page</title>
    <style>
        body {
            background-color: #1a1a1a;
            font-family: 'Courier New', monospace;
            margin-top: 20px;
        }

        h1 {
            color: #ffcc00;
        }

        .settings-table {
            display: grid;
            grid-template-columns: auto auto;
            gap: 10px;
            justify-content: center;
            margin-top: 20px;
        }

        .setting {
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 10px;
            background-color: #2c3e50;
            border-radius: 5px;
        }

        .description {
            color: white;
            margin-right: 20px;
            width: 30%;
            text-align: right;
        }

        .setting-container {
            display: flex;
            align-items: center;
            margin-top: 10px;
        }

        .button-group {
            display: flex;
            justify-content: left;
            width: 70%;
        }

        .button {
            height: 34px;
            width: 48px;
            margin: 7px;
            color: grey;
            text-align: center;
            position: relative;
            padding: 0;
            background-color: #2c3e50;
            color: #ecf0f1;
            border: none;
            cursor: pointer;
            border-radius: 5px;
        }

        .button.active {
            background-color: #3498db;
        }

        .redButton {
            padding: 8px 15px;
            background-color: #e74c3c;
            color: #ecf0f1;
            border-radius: 5px;
            border: none;
        }
        .orangeButton {
            padding: 8px 15px;
            background-color: #f39c12;
            color: #ecf0f1;
            border-radius: 5px;
            border: none;
        }
        .section-header {
            grid-column: 1 / -1;
            color: #ffcc00;
            font-size: 1.2em;
            margin-top: 20px;
            margin-bottom: 10px;
            text-align: center;
            border-bottom: 1px solid #ffcc00;
            padding-bottom: 5px;
        }
        .fab {
            position: fixed;
            bottom: 20px;
            right: 20px;
            width: 60px;
            height: 60px;
            border-radius: 50%;
            background-color: #27ae60;
            color: white;
            font-size: 30px;
            border: none;
            box-shadow: 0 4px 8px rgba(0,0,0,0.3);
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: transform 0.2s;
        }
        .fab:active {
            transform: scale(0.95);
        }
        .toast {
            visibility: hidden;
            min-width: 250px;
            background-color: #333;
            color: #fff;
            text-align: center;
            border-radius: 2px;
            padding: 16px;
            position: fixed;
            z-index: 1;
            left: 50%;
            bottom: 30px;
            transform: translateX(-50%);
            font-size: 17px;
        }
        .toast.show {
            visibility: visible;
            -webkit-animation: fadein 0.5s, fadeout 0.5s 2.5s;
            animation: fadein 0.5s, fadeout 0.5s 2.5s;
        }
        @-webkit-keyframes fadein {
            from {bottom: 0; opacity: 0;}
            to {bottom: 30px; opacity: 1;}
        }
        @keyframes fadein {
            from {bottom: 0; opacity: 0;}
            to {bottom: 30px; opacity: 1;}
        }
        @-webkit-keyframes fadeout {
            from {bottom: 30px; opacity: 1;}
            to {bottom: 0; opacity: 0;}
        }
        @keyframes fadeout {
            from {bottom: 30px; opacity: 1;}
            to {bottom: 0; opacity: 0;}
        }
    </style>
    <script>
        const socket = new WebSocket('ws://' + window.location.hostname + '/WebSocketSettings');
        let originalSettings = {};
        let currentSettings = {};

        socket.onopen = function (event) {
            socket.send('get');
        };
        socket.onclose = function (event) {
            console.log('Config WebSocket closed');
        };
        socket.onmessage = function (event) {
            const response = JSON.parse(event.data);

            // If this is the first load, save as original
            if (Object.keys(originalSettings).length === 0) {
                originalSettings = {...response};
            }
            // Always update current settings
            currentSettings = {...response};

            updateUI(response);
        };

        function updateUI(settings) {
            setActiveButton('.speedButton', settings['speed']);
            setActiveButton('.readSamplesButton', settings['read_samples']);
            setActiveButton('.gainButton', settings['gain']);

            setInputValue('calibration_factor', settings['calibration_factor']);
            setInputValue('target_dose_single', settings['target_dose_single']);
            setInputValue('target_dose_double', settings['target_dose_double']);
            setInputValue('top_up_margin_single', settings['top_up_margin_single']);
            setInputValue('top_up_margin_double', settings['top_up_margin_double']);
            setInputValue('min_topup_grams', settings['min_topup_grams']);
            setInputValue('topup_timeout_ms', settings['topup_timeout_ms']);
            setInputValue('grinding_timeout_ms', settings['grinding_timeout_ms']);
            setInputValue('finalize_timeout_ms', settings['finalize_timeout_ms']);
            setInputValue('confirm_timeout_ms', settings['confirm_timeout_ms']);
            setInputValue('stability_min_wait_ms', settings['stability_min_wait_ms']);
            setInputValue('stability_max_wait_ms', settings['stability_max_wait_ms']);
            setInputValue('button_debounce_ms', settings['button_debounce_ms']);
        }

        function setInputValue(id, value) {
            const el = document.getElementById(id);
            if (el) el.value = value;
        }

        function setActiveButton(className, value) {
            const buttons = document.querySelectorAll(className);
            buttons.forEach(button => {
                button.classList.remove('active');
                if (button.getAttribute('data-value') == value) {
                    button.classList.add('active');
                }
            });
        }

        function updateValue(key, value) {
            currentSettings[key] = value;
            // Update UI immediately for buttons
            if (key === 'speed') setActiveButton('.speedButton', value);
            if (key === 'read_samples') setActiveButton('.readSamplesButton', value);
            if (key === 'gain') setActiveButton('.gainButton', value);
        }

        function saveSettings() {
            let diff = {};
            let hasChanges = false;
            for (let key in currentSettings) {
                // Simple comparison, might need type conversion if types mismatch
                if (currentSettings[key] != originalSettings[key]) {
                    diff[key] = currentSettings[key];
                    hasChanges = true;
                }
            }

            if (hasChanges) {
                socket.send('batch:' + JSON.stringify(diff));
                // Update original settings to match current
                originalSettings = {...currentSettings};
                showToast("Settings Saved!");
            } else {
                showToast("No changes to save");
            }
        }

        function resetWiFi() {
            if(confirm("Reset WiFi settings and reboot?")) {
                socket.send('batch:{"resetWiFi":true}');
            }
        }

        function rebootDevice() {
            if(confirm("Reboot device?")) {
                socket.send('batch:{"reboot":true}');
            }
        }

        function showToast(message) {
            var x = document.getElementById("toast");
            x.innerText = message;
            x.className = "toast show";
            setTimeout(function(){ x.className = x.className.replace("show", ""); }, 3000);
        }
    </script>
</head>

<body>
    <h1>Config Page</h1>
    <div class="setting-container">
        <div class="description">Samples per ADC read</div>
        <div class="button-group">
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 1)" data-value="1">1</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 2)" data-value="2">2</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 4)" data-value="4">4</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 8)" data-value="8">8</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 12)" data-value="12">12</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 24)" data-value="24">24</button>
            <button class="button readSamplesButton" onclick="updateValue('read_samples', 48)" data-value="48">48</button>
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Speed (SPS)</div>
        <div class="button-group">
            <button class="button speedButton" onclick="updateValue('speed', 10)" data-value="10">10</button>
            <button class="button speedButton" onclick="updateValue('speed', 80)" data-value="80">80</button>
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Gain</div>
        <div class="button-group">
            <button class="button gainButton" onclick="updateValue('gain', 1)" data-value="1">1</button>
            <button class="button gainButton" onclick="updateValue('gain', 2)" data-value="2">2</button>
            <button class="button gainButton" onclick="updateValue('gain', 64)" data-value="64">64</button>
            <button class="button gainButton" onclick="updateValue('gain', 128)" data-value="128">128</button>
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Calibration Factor</div>
        <div class="text-input">
            <input type="text" id="calibration_factor" placeholder="Enter value" oninput="updateValue('calibration_factor', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Target Dose Single [g]</div>
        <div class="text-input">
            <input type="text" id="target_dose_single" placeholder="Enter value" oninput="updateValue('target_dose_single', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Top Up Margin Single [g]</div>
        <div class="text-input">
            <input type="text" id="top_up_margin_single" placeholder="Enter value" oninput="updateValue('top_up_margin_single', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Target Dose Double [g]</div>
        <div class="text-input">
            <input type="text" id="target_dose_double" placeholder="Enter value" oninput="updateValue('target_dose_double', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Top Up Margin Double [g]</div>
        <div class="text-input">
            <input type="text" id="top_up_margin_double" placeholder="Enter value" oninput="updateValue('top_up_margin_double', this.value)">
        </div>
    </div>

    <div class="section-header">Advanced Config</div>

    <div class="setting-container">
        <div class="description">Min Top Up [g]</div>
        <div class="text-input">
            <input type="text" id="min_topup_grams" placeholder="Enter value" oninput="updateValue('min_topup_grams', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Top Up Timeout [ms]</div>
        <div class="text-input">
            <input type="text" id="topup_timeout_ms" placeholder="Enter value" oninput="updateValue('topup_timeout_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Grinding Timeout [ms]</div>
        <div class="text-input">
            <input type="text" id="grinding_timeout_ms" placeholder="Enter value" oninput="updateValue('grinding_timeout_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Finalize Timeout [ms]</div>
        <div class="text-input">
            <input type="text" id="finalize_timeout_ms" placeholder="Enter value" oninput="updateValue('finalize_timeout_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Confirm Timeout [ms]</div>
        <div class="text-input">
            <input type="text" id="confirm_timeout_ms" placeholder="Enter value" oninput="updateValue('confirm_timeout_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Stability Min Wait [ms]</div>
        <div class="text-input">
            <input type="text" id="stability_min_wait_ms" placeholder="Enter value" oninput="updateValue('stability_min_wait_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Stability Max Wait [ms]</div>
        <div class="text-input">
            <input type="text" id="stability_max_wait_ms" placeholder="Enter value" oninput="updateValue('stability_max_wait_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Button Debounce [ms]</div>
        <div class="text-input">
            <input type="text" id="button_debounce_ms" placeholder="Enter value" oninput="updateValue('button_debounce_ms', this.value)">
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Reset WiFi</div>
        <div class="button-group">
            <button class="redButton" onclick="resetWiFi()">Reset WiFi</button>
        </div>
    </div>
    <div class="setting-container">
        <div class="description">Reboot Device</div>
        <div class="button-group">
            <button class="orangeButton" onclick="rebootDevice()">Reboot</button>
        </div>
    </div>

    <button class="fab" onclick="saveSettings()">💾</button>
    <div id="toast" class="toast">Settings Saved!</div>
</body>

</html>
)rawliteral";

void onConnect(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", websettings::config_html);
}
} // namespace websettings

const int WebSocketSettings::EEPROM_SCALE_ADDRESS = 0;

WebSocketSettings::WebSocketSettings()
    : _ws("/WebSocketSettings"), _server(nullptr), _logger(nullptr) {}

void WebSocketSettings::begin(AsyncWebServer *srv,
                              const WebSocketLogger *logger) {
  _server = srv;
  _server->addHandler(&_ws);
  _server->on("/settings", HTTP_GET, websettings::onConnect);
  _logger = logger;
  _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    this->onWebSocketEvent(server, client, type, arg, data, len);
  });

  loadScaleFromEEPROM();
}

void WebSocketSettings::onWebSocketEvent(AsyncWebSocket *server,
                                         AsyncWebSocketClient *client,
                                         AwsEventType type, void *arg,
                                         uint8_t *data, size_t len) {
  switch (type) {
  case WS_EVT_CONNECT:
    // Limit to single connection for security - settings should have exclusive access
    if (server->count() > 1) {
      client->close(1008, "Only one settings connection allowed");
    }
    _logger->println("WebSocket client connected");
    break;
  case WS_EVT_DISCONNECT:
    _logger->println("WebSocket client disconnected");
    break;
  case WS_EVT_DATA:
    if (arg) {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->opcode == WS_TEXT) {
        String cmd = String((char *)data);
        String response;
        handleWebSocketText(cmd, response);
        _logger->println("Send response: " + response);
        client->text(response.c_str());
      }
    }
    break;
  }
}

void WebSocketSettings::handleWebSocketText(const String &cmd,
                                            String &response) {
  _logger->println(String("Handle cmd: " + cmd));

  if (cmd.startsWith("batch:")) {
    String jsonStr = cmd.substring(6);
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (!error) {
      JsonObject obj = doc.as<JsonObject>();
      bool changed = false;

      if (obj.containsKey("read_samples")) { scale.read_samples = obj["read_samples"]; changed = true; }
      if (obj.containsKey("speed")) { scale.speed = obj["speed"]; changed = true; }
      if (obj.containsKey("gain")) { scale.gain = obj["gain"]; changed = true; }
      if (obj.containsKey("calibration_factor")) { scale.calibration_factor = obj["calibration_factor"]; changed = true; }
      if (obj.containsKey("target_dose_single")) { scale.target_dose_single = obj["target_dose_single"]; changed = true; }
      if (obj.containsKey("target_dose_double")) { scale.target_dose_double = obj["target_dose_double"]; changed = true; }
      if (obj.containsKey("top_up_margin_single")) { scale.top_up_margin_single = obj["top_up_margin_single"]; changed = true; }
      if (obj.containsKey("top_up_margin_double")) { scale.top_up_margin_double = obj["top_up_margin_double"]; changed = true; }
      if (obj.containsKey("min_topup_grams")) { scale.min_topup_grams = obj["min_topup_grams"]; changed = true; }
      if (obj.containsKey("topup_timeout_ms")) { scale.topup_timeout_ms = obj["topup_timeout_ms"]; changed = true; }
      if (obj.containsKey("grinding_timeout_ms")) { scale.grinding_timeout_ms = obj["grinding_timeout_ms"]; changed = true; }
      if (obj.containsKey("finalize_timeout_ms")) { scale.finalize_timeout_ms = obj["finalize_timeout_ms"]; changed = true; }
      if (obj.containsKey("confirm_timeout_ms")) { scale.confirm_timeout_ms = obj["confirm_timeout_ms"]; changed = true; }
      if (obj.containsKey("stability_min_wait_ms")) { scale.stability_min_wait_ms = obj["stability_min_wait_ms"]; changed = true; }
      if (obj.containsKey("stability_max_wait_ms")) { scale.stability_max_wait_ms = obj["stability_max_wait_ms"]; changed = true; }
      if (obj.containsKey("button_debounce_ms")) { scale.button_debounce_ms = obj["button_debounce_ms"]; changed = true; }

      if (obj.containsKey("resetWiFi") && obj["resetWiFi"]) { wifi.reset_flag = true; changed = true; }
      if (obj.containsKey("reboot") && obj["reboot"]) { wifi.reboot_flag = true; changed = true; }

      if (changed) {
        saveScaleToEEPROM();
        scale.is_changed = true;
      }
    }
  } else {
    // Extract command parts
    int cIdx = cmd.indexOf(':');
    String cmdType = cmd.substring(0, cIdx);
    String varName = cmd.substring(cIdx + 1, cmd.indexOf(':', cIdx + 1));
    String value = cmd.substring(cmd.indexOf(':', cIdx + 1) + 1);

    if (cmdType == "set") {
      if (varName == "read_samples") {
        scale.read_samples = value.toInt();
      } else if (varName == "speed") {
        scale.speed = value.toInt();
      } else if (varName == "gain") {
        scale.gain = value.toInt();
      } else if (varName == "calibration_factor") {
        scale.calibration_factor = value.toFloat();
      } else if (varName == "target_dose_single") {
        scale.target_dose_single = value.toFloat();
      } else if (varName == "target_dose_double") {
        scale.target_dose_double = value.toFloat();
      } else if (varName == "top_up_margin_single") {
        scale.top_up_margin_single = value.toFloat();
      } else if (varName == "top_up_margin_double") {
        scale.top_up_margin_double = value.toFloat();
      } else if (varName == "min_topup_grams") {
        scale.min_topup_grams = value.toFloat();
      } else if (varName == "topup_timeout_ms") {
        scale.topup_timeout_ms = value.toInt();
      } else if (varName == "grinding_timeout_ms") {
        scale.grinding_timeout_ms = value.toInt();
      } else if (varName == "finalize_timeout_ms") {
        scale.finalize_timeout_ms = value.toInt();
      } else if (varName == "confirm_timeout_ms") {
        scale.confirm_timeout_ms = value.toInt();
      } else if (varName == "stability_min_wait_ms") {
        scale.stability_min_wait_ms = value.toInt();
      } else if (varName == "stability_max_wait_ms") {
        scale.stability_max_wait_ms = value.toInt();
      } else if (varName == "button_debounce_ms") {
        scale.button_debounce_ms = value.toInt();
      } else if (varName == "resetWiFi") {
        wifi.reset_flag = true;
      }

      saveScaleToEEPROM();
      scale.is_changed = true;
    }
  }

  // Response contains all settings
  StaticJsonDocument<512> jsonDoc;
  char cds_rounded[8], cdd_rounded[8], min_topup_rounded[8];
  sprintf(cds_rounded, "%1.2f", scale.top_up_margin_single);
  sprintf(cdd_rounded, "%1.2f", scale.top_up_margin_double);
  sprintf(min_topup_rounded, "%1.2f", scale.min_topup_grams);
  jsonDoc["read_samples"] = scale.read_samples;
  jsonDoc["speed"] = scale.speed;
  jsonDoc["gain"] = scale.gain;
  jsonDoc["calibration_factor"] = scale.calibration_factor;
  jsonDoc["target_dose_single"] = scale.target_dose_single;
  jsonDoc["target_dose_double"] = scale.target_dose_double;
  jsonDoc["top_up_margin_single"] = cds_rounded;
  jsonDoc["top_up_margin_double"] = cdd_rounded;
  jsonDoc["min_topup_grams"] = min_topup_rounded;
  jsonDoc["topup_timeout_ms"] = scale.topup_timeout_ms;
  jsonDoc["grinding_timeout_ms"] = scale.grinding_timeout_ms;
  jsonDoc["finalize_timeout_ms"] = scale.finalize_timeout_ms;
  jsonDoc["confirm_timeout_ms"] = scale.confirm_timeout_ms;
  jsonDoc["stability_min_wait_ms"] = scale.stability_min_wait_ms;
  jsonDoc["stability_max_wait_ms"] = scale.stability_max_wait_ms;
  jsonDoc["button_debounce_ms"] = scale.button_debounce_ms;

  serializeJson(jsonDoc, response);
}

void WebSocketSettings::loadScaleFromEEPROM() {
  EEPROM.begin(sizeof(scale));
  EEPROM.get(EEPROM_SCALE_ADDRESS, scale);
  EEPROM.end();
}

void WebSocketSettings::saveScaleToEEPROM() {
  EEPROM.begin(sizeof(scale));
  EEPROM.put(EEPROM_SCALE_ADDRESS, scale);
  bool success = EEPROM.commit();
  if (!success) {
    _logger->println("EEPROM.commit error");
  }
  EEPROM.end();
}