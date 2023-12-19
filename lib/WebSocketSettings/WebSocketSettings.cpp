
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
    height:34px;
    width:48px;
    margin: 7px;
    color:grey;
    text-align:center;
    position:relative;
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
  </style>
  <script>
    const socket = new WebSocket('ws://' + window.location.hostname + '/WebSocketSettings');
    socket.onopen = function (event) {
      socket.send('get');
    };
    socket.onclose = function (event) {
      console.log('Config WebSocket closed');
    };
    socket.onmessage = function (event) {
      const response = JSON.parse(event.data);
      setActiveButton('.speedButton', response['speed']);
      setActiveButton('.readSamplesButton', response['read_samples']);
      setActiveButton('.gainButton', response['gain']);
    };
    function setActiveButton(className, value) {
      const buttons = document.querySelectorAll(className);
      buttons.forEach(button => {
        button.classList.remove('active');
        if (button.getAttribute('data-value') == value) {
          button.classList.add('active');
        }
      });
    }
    function set(variable, value) {
      socket.send('set:' + variable + ':' + value);
    }
  </script>
</head>
<body>
  <h1>Config Page</h1>
  <div class="setting-container">
    <div class="description">Samples per ADC read</div>
    <div class="button-group">
      <button class="button readSamplesButton" onclick="set('read_samples', '1')" data-value="1">1</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '2')" data-value="2">2</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '4')" data-value="4">4</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '8')" data-value="8">8</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '12')" data-value="12">12</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '24')" data-value="24">24</button>
      <button class="button readSamplesButton" onclick="set('read_samples', '48')" data-value="48">48</button>
    </div>
  </div>
  <div class="setting-container">
    <div class="description">Speed (SPS)</div>
    <div class="button-group">
      <button class="button speedButton" onclick="set('speed', '10')" data-value="10">10</button>
      <button class="button speedButton" onclick="set('speed', '80')" data-value="80">80</button>
    </div>
  </div>
  <div class="setting-container">
    <div class="description">Gain</div>
    <div class="button-group">
      <button class="button gainButton" onclick="set('gain', '1')" data-value="1">1</button>
      <button class="button gainButton" onclick="set('gain', '2')" data-value="2">2</button>
      <button class="button gainButton" onclick="set('gain', '64')" data-value="64">64</button>
      <button class="button gainButton" onclick="set('gain', '128')" data-value="128">128</button>
    </div>
  </div>
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
  // Extract command parts
  int cIdx = cmd.indexOf(':');
  String cmdType = cmd.substring(0, cIdx);
  String varName = cmd.substring(cIdx + 1, cmd.indexOf(':', cIdx + 1));
  String value = cmd.substring(cmd.indexOf(':', cIdx + 1) + 1);

  _logger->println(String("Handle cmd: " + cmd));
  if (cmdType == "set") {
    if (varName == "read_samples") {
      scale.read_samples = value.toInt();
    } else if (varName == "speed") {
      scale.speed = value.toInt();
    } else if (varName == "gain") {
      scale.gain = value.toInt();
    }

    saveScaleToEEPROM();
    scale.is_changed = true;
  }

  // Response contains all settings
  StaticJsonDocument<128> jsonDoc;
  jsonDoc["read_samples"] = scale.read_samples;
  jsonDoc["speed"] = scale.speed;
  jsonDoc["gain"] = scale.gain;

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