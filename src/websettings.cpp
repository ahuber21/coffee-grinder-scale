#include "websettings.h"
#include <ESPAsyncWebServer.h>

namespace websettings {
const char PROGMEM config_html[] = R"rawliteral(
      <!DOCTYPE html>
      <html lang="en">
      <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Configuration Page</title>
        <script>
          const socket = new WebSocket('ws://' + window.location.hostname + '/config-ws');
          socket.onopen = function(event) {
            console.log('Config WebSocket opened');
          };
          socket.onclose = function(event) {
            console.log('Config WebSocket closed');
          };
          function changeScaleReadSamples() {
            const selectElement = document.getElementById('scaleSelect');
            const selectedValue = selectElement.options[selectElement.selectedIndex].value;
            socket.send(selectedValue);
          }
        </script>
      </head>
      <body>
        <h1>Configuration Page</h1>
        <label for="scaleSelect">Select Scale Read Samples:</label>
        <select id="scaleSelect" onchange="changeScaleReadSamples()">
          <option value="4">4</option>
          <option value="8">8</option>
          <option value="12">12</option>
          <option value="24">24</option>
          <option value="48">48</option>
        </select>
      </body>
      </html>
)rawliteral";

void onConnect(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", websettings::config_html);
}

void handleWebSocketData(AsyncWebSocketClient *client, uint8_t *data,
                         size_t len) {
  // Process the received data, for example, interpret it as a string
  String message = "";
  for (size_t i = 0; i < len; i++) {
    message += (char)data[i];
  }

  // Log the message to the Serial and send it back to the WebSocket client
  Serial.println("Received message: " + message);
  client->text("Echo: " + message);
}

void onWebSocketEvent(WebSettings *settings, AsyncWebSocket *server,
                      AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len) {
  switch (type) {
  case WS_EVT_CONNECT:
    settings->println("WebSocket client connected");
    break;
  case WS_EVT_DISCONNECT:
    settings->println("WebSocket client disconnected");
    break;
  case WS_EVT_DATA:
    if (arg) {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->opcode == WS_TEXT) {
        // Change the scale_read_samples variable based on the received
        // WebSocket message
        // scale_read_samples = atoi((char *)data);
        settings->print("Scale Read Samples changed to: ");
        // Serial.println(scale_read_samples);
      }
    }
    break;
  }
}
} // namespace websettings

void WebSettings::begin(AsyncWebServer *srv, const WSLogger *logger) {
  _logger = logger;
  _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    websettings::onWebSocketEvent(this, server, client, type, arg, data, len);
  });
  _server = srv;
  _server->addHandler(&_ws);
  _server->on("/settings", HTTP_GET, websettings::onConnect);
}