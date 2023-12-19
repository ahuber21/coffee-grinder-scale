
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
    text-align: center;
    margin-top: 50px;
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
  }
  .button-container {
    display: flex;
    justify-content: center;
    align-items: center;
  }
  .button-group {
      display: flex;
      justify-content: center;
    }
  .button {
    background-color: #2c3e50;
    color: #ecf0f1;
    border: none;
    padding: 10px 20px;
    margin: 5px;
    cursor: pointer;
    border-radius: 5px;
    display: flex;
    align-items: center;
    text-align: center;
  }
  .button.active {
    background-color: #3498db;
  }
  </style>
  <script>
    const socket = new WebSocket('ws://' + window.location.hostname + '/WebSocketSettings');
    socket.onopen = function (event) {
      socket.send('get:read_samples:');
    };
    socket.onclose = function (event) {
      console.log('Config WebSocket closed');
    };
    socket.onmessage = function (event) {
      const response = JSON.parse(event.data);
      if (response.variable && response.value !== undefined) {
        setActiveButton(response.value);
      }
    };
    function changeScaleReadSamples(value) {
      socket.send('set:read_samples:' + value);
      setActiveButton(value);
    }
    function setActiveButton(value) {
      const buttons = document.querySelectorAll('.button');
      buttons.forEach(button => {
        button.classList.remove('active');
        if (button.getAttribute('data-value') == value) {
          button.classList.add('active');
        }
      });
    }
  </script>
</head>
<body>
  <h1>Config Page</h1>
  <div class="button-container">
    <div class="description">Samples per ADC read</div>
    <div class="button-group">
      <button class="button" onclick="changeScaleReadSamples('4')" data-value="4">4</button>
      <button class="button" onclick="changeScaleReadSamples('8')" data-value="8">8</button>
      <button class="button" onclick="changeScaleReadSamples('12')" data-value="12">12</button>
      <button class="button" onclick="changeScaleReadSamples('24')" data-value="24">24</button>
      <button class="button" onclick="changeScaleReadSamples('48')" data-value="48">48</button>
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
        if (handleWebSocketText(cmd, response))
          client->text(response.c_str());
      }
    }
    break;
  }
}

bool WebSocketSettings::handleWebSocketText(const String &cmd,
                                            String &response) {
  // Split the command string using the colon as a delimiter
  int colonIndex = cmd.indexOf(':');
  if (colonIndex == -1) {
    // Invalid command format
    return false;
  }

  // Extract command parts
  String cmdType = cmd.substring(0, colonIndex);
  String varName =
      cmd.substring(colonIndex + 1, cmd.indexOf(':', colonIndex + 1));
  String value = cmd.substring(cmd.indexOf(':', colonIndex + 1) + 1);

  // Prepare JSON response object
  StaticJsonDocument<128> jsonDoc; // Adjust the capacity as needed
  jsonDoc["variable"] = varName;

  // Handle the command based on its type and variable name
  if (cmdType == "get") {
    // Get command, handle accordingly
    if (varName == "read_samples") {
      jsonDoc["value"] = scale.read_samples;
    }

    serializeJson(jsonDoc, response);
    return true;
  }

  if (cmdType == "set") {
    // Set command, handle accordingly
    if (varName == "read_samples") {
      // Convert the value to byte and set it
      scale.read_samples = value.toInt();
      // Save the updated scale to EEPROM
      saveScaleToEEPROM();
      _logger->println(String("read_samples set to: " + value));
    }
  }

  return false;
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