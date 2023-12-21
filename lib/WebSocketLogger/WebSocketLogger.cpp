#include "WebSocketLogger.h"
#include <ESPAsyncWebServer.h>

namespace wslogger {
const char PROGMEM index_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WebSocket Client</title>
  <style>
    body {
      background-color: #1a1a1a; /* Dark background */
      color: white; /* White text color */
      font-family: 'Courier New', monospace; /* Monospace font */
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      height: 100vh;
    }

    h1 {
      color: #ffcc00; /* Yellowish heading color */
    }

    #messagesContainer {
      flex-grow: 1;
      overflow-y: auto;
      font-size: 16px;
      line-height: 1.5;
      scrollbar-width: thin; /* Always show the scrollbar */
    }

    .timestamp {
      font-weight: bold; /* Bold font for timestamp */
      color: #66ccff; /* Light blue color for timestamp */
    }

    #inputContainer {
      display: flex;
      align-items: center;
      padding: 10px;
      background-color: #333; /* Darker background for the input area */
      margin-right: 10px; /* Adjust margin for scrollbar */
    }

    #messageInput {
      flex-grow: 1;
      margin-right: 10px;
      padding: 5px;
      font-size: 16px;
    }

    #sendMessageButton {
      padding: 5px 10px;
      font-size: 16px;
      cursor: pointer;
      background-color: #66ccff; /* Light blue color for the button */
      color: #1a1a1a; /* Dark text color for the button */
      border: none;
    }
  </style>
</head>
<body>
  <h1>WebSocket Client</h1>
  <div id="messagesContainer"></div>
  <div id="inputContainer">
    <input type="text" id="messageInput" placeholder="Type your message...">
    <button id="sendMessageButton">Send</button>
  </div>

<script>
  const socket = new WebSocket('ws://' + window.location.hostname + '/WebSocketLogger');
  const messagesContainer = document.getElementById('messagesContainer');
  const messageInput = document.getElementById('messageInput');
  const sendMessageButton = document.getElementById('sendMessageButton');

  socket.onopen = function(event) {
    addMessage('WebSocket opened');
  };

  socket.onmessage = function(event) {
    addMessage(event.data);
    // Automatically scroll down when a new message is added
    messagesContainer.scrollTop = messagesContainer.scrollHeight;
  };

  socket.onclose = function(event) {
    addMessage('WebSocket closed');
  };

  function sendMessage() {
    const messageToSend = messageInput.value;
    if (messageToSend.trim() !== '') {
      socket.send(messageToSend);
      messageInput.value = '';
    }
  }

  sendMessageButton.addEventListener('click', sendMessage);

  messageInput.addEventListener('keydown', function(event) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault(); // Prevents the default behavior (line break)
      sendMessage();
    }
  });

  function addMessage(message) {
    const messageDiv = document.createElement('div');
    messageDiv.innerHTML = getCurrentDateTime() + ': ' + message.replace(/\n/g, '<br>');
    messagesContainer.appendChild(messageDiv);
  }

  function getCurrentDateTime() {
    const now = new Date();
    const dateString = now.toISOString().slice(0, 19).replace("T", " ");
    const milliseconds = now.getMilliseconds().toString().padStart(3, '0');
    return '<span class="timestamp">' + dateString + '.' + milliseconds + '</span>';
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
  client->text("Welcome to the Eureka web socket logger");
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

WebSocketLogger::WebSocketLogger() : _ws("/WebSocketLogger") {}

void WebSocketLogger::begin(AsyncWebServer *srv) {
  _ws.onEvent(&wslogger::onWebSocketEvent);
  _server = srv;
  _server->addHandler(&_ws);
  _server->on("/console", HTTP_GET, wslogger::onConnect);
}

void WebSocketLogger::print(const String &message) const {
  Serial.print(message);
  for (auto *client : _ws.getClients()) {
    if (client->status() == WS_CONNECTED) {
      client->text(message);
    }
  }
}
