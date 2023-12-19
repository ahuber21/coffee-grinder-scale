
#include "WebSocketSettings.h"
#include "WebSocketLogger.h"
#include <ESPAsyncWebServer.h>

namespace websettings {
const char PROGMEM config_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Configuration Page</title>
  <style>
    body {
      background-color: #2c3e50;
      color: #ecf0f1;
      font-family: Arial, sans-serif;
      text-align: center;
      margin-top: 50px;
    }

    h1 {
      color: #3498db;
    }

    .button-group {
      display: flex;
      justify-content: center;
      margin-top: 20px;
    }

    .button-container {
      display: flex;
      align-items: center;
      margin-bottom: 10px;
    }

    .description {
      margin-right: 10px;
    }

    .button {
      background-color: #34495e;
      color: #ecf0f1;
      border: none;
      padding: 10px 20px;
      margin: 0 5px;
      cursor: pointer;
      border-radius: 5px;
    }

    .button.active {
      background-color: #3498db;
    }
  </style>
  <script>
    const socket = new WebSocket('ws://' + window.location.hostname + '/WebSocketSettings');
    socket.onopen = function(event) {
      console.log('Config WebSocket opened');
    };
    socket.onclose = function(event) {
      console.log('Config WebSocket closed');
    };

    function changeScaleReadSamples(value) {
      socket.send(value);
      setActiveButton(value);
    }

    function setActiveButton(value) {
      const buttons = document.querySelectorAll('.button');
      buttons.forEach(button => {
        button.classList.remove('active');
        if (button.getAttribute('data-value') === value) {
          button.classList.add('active');
        }
      });
    }
  </script>
</head>
<body>
  <h1>Configuration Page</h1>
  <div class="button-container">
    <div class="description">Number of samples per ADC read:</div>
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
        scale.read_samples = atoi((char *)data);
        _logger->print("Scale Read Samples changed to: ");
        _logger->println(scale.read_samples);
      }
    }
    break;
  }
}