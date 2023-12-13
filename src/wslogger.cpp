#include "wslogger.h"
// #include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

namespace wslogger {
const char PROGMEM index_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WebSocket Client</title>
</head>
<body>
  <h1>WebSocket Client</h1>
  <div id="messagesContainer"></div>

  <script>
    const socket = new WebSocket('ws://' + window.location.hostname + '/ws');
    const messagesContainer = document.getElementById('messagesContainer');

    socket.onopen = function(event) {
      addMessage('WebSocket opened');
    };

    socket.onmessage = function(event) {
      addMessage(event.data);
    };

    socket.onclose = function(event) {
      addMessage('WebSocket closed');
    };

    function addMessage(message) {
      const messageDiv = document.createElement('div');
      messageDiv.textContent = getCurrentDateTime() + ': ' + message;
      messagesContainer.appendChild(messageDiv);
    }

    function getCurrentDateTime() {
      const now = new Date();
      const dateString = now.toISOString().slice(0, 19).replace("T", " ");
      const milliseconds = now.getMilliseconds();
      return dateString + '.' + milliseconds;
    }
  </script>
</body>
</html>
)rawliteral";

void onConnect(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", index_html);
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
void sendWelcomeMessage(AsyncWebSocketClient *client) {
  // Send a welcome message to the newly connected client
  client->text(WS_WELCOME_MSG);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
  case WS_EVT_CONNECT:
    Serial.println("WebSocket client connected");
    sendWelcomeMessage(client);
    break;
  case WS_EVT_DISCONNECT:
    Serial.println("WebSocket client disconnected");
    break;
  case WS_EVT_DATA:
    handleWebSocketData(client, data, len);
    break;
  case WS_EVT_PONG:
    break;
  case WS_EVT_ERROR:
    break;
  }
}
} // namespace wslogger

void WSLogger::begin(AsyncWebServer *srv) {
  _ws.onEvent(&wslogger::onWebSocketEvent);
  _server = srv;
  _server->addHandler(&_ws);
  _server->on("/console", HTTP_GET, wslogger::onConnect);
}

void WSLogger::print(const String &message) {
  Serial.print(message);
  for (auto *client : _ws.getClients()) {
    if (client->status() == WS_CONNECTED) {
      client->text(message);
    }
  }
}
