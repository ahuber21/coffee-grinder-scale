#include "WebSocketGraph.h"

const char PROGMEM WebSocketGraph::graph_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Graph Page</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<style>
  body {
    background-color: #1a1a1a;
    font-family: 'Courier New', monospace;
    margin-top: 20px;
  }
  h1 {
    color: #ffcc00;
  }
  .status {
    display: flex;
    align-items: center;
    margin-top: 10px;
  }
  .circle {
    width: 15px;
    height: 15px;
    border-radius: 50%;
    margin-right: 10px;
  }
  #statusText {
    color: white;
  }
  canvas {
    background-color: #34495e;
    border-radius: 5px;
    margin-top: 20px;
  }
</style>
<body>
  <h1>Graph Page</h1>
  <div class="status">
    <div id="statusCircle" class="circle"></div>
    <div id="statusText">DISCONNECTED</div>
  </div>
  <canvas id="graphCanvas" width="800" height="400"></canvas>

  <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
  <script>
    const socket = new WebSocket('ws://' + window.location.hostname + '/GraphWebSocket');

    let weightChart;
    let weightDataset;
    function createChart() {
        // recreate graphCanvas to reset all contents
        const existingCanvas = document.getElementById('graphCanvas');
        existingCanvas.parentNode.removeChild(existingCanvas);
        const newCanvas = document.createElement('canvas');
        newCanvas.id = 'graphCanvas';
        document.body.appendChild(newCanvas);

        const ctx = document.getElementById('graphCanvas');

        weightDataset = {
            label: 'Weight',
            borderColor: '#3498db',
            borderWidth: 2,
            fill: false,
            data: [],
        };

        weightChart = new Chart(ctx, {
        type: 'line',
        data: {
            datasets: [weightDataset],
        },
        options: {
            responsive: true,
            scales: {
            x: {
                type: 'linear',
                position: 'bottom',
                title: {
                display: true,
                text: 'Time [s]',
                font: {
                    size: 14,
                    color: 'white', // Set text color to white
                },
                },
                ticks: {
                color: 'white', // Set tick color to white
                },
            },
            y: {
                type: 'linear',
                position: 'left',
                title: {
                display: true,
                text: 'Weight [g]',
                font: {
                    size: 14,
                    color: 'white', // Set text color to white
                },
                },
                ticks: {
                color: 'white', // Set tick color to white
                },
            },
            },
            elements: {
            point: {
                backgroundColor: '#ffcc00', // Set circle color to yellow
                borderColor: '#ffcc00', // Set circle border color to yellow
            },
            line: {
                backgroundColor: '#ffcc00', // Set line color to yellow
                borderColor: '#ffcc00', // Set line border color to yellow
            },
            },
            plugins: {
            legend: {
                display: false, // Hide legend
            },
            },
        },
        });
    }

    const statusText = document.getElementById('statusText');
    const statusCircle = document.getElementById('statusCircle');;

    socket.onopen = function () {
      updateConnectionStatus(true);
      createChart();
    };

    socket.onclose = function () {
      updateConnectionStatus(false);
    };

    socket.onmessage = function (event) {
        const data = JSON.parse(event.data);
        if (event.target_weight) {
            createChart();
        } else {
            weightDataset.data.push({ x: data.seconds, y: data.weight });
            weightChart.update();
        }
    };

    function updateConnectionStatus(connected) {
      if (connected) {
        statusCircle.style.backgroundColor = '#2ecc71';
        statusText.textContent = 'CONNECTED';
      } else {
        statusCircle.style.backgroundColor = '#e74c3c';
        statusText.textContent = 'DISCONNECTED';
      }
    }
  </script>
</body>
</html>
)rawliteral";

WebSocketGraph::WebSocketGraph() : _ws("/GraphWebSocket"), _server(nullptr) {}

void WebSocketGraph::begin(AsyncWebServer *server) {
  _server = server;
  _server->addHandler(&_ws);
  _server->on("/graph", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", graph_html);
  });

  _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    this->handleWebSocketEvent(server, client, type, arg, data, len);
  });
}

void WebSocketGraph::handleWebSocketEvent(AsyncWebSocket *server,
                                          AsyncWebSocketClient *client,
                                          AwsEventType type, void *arg,
                                          uint8_t *data, size_t len) {
  // Handle WebSocket events if needed
}

void WebSocketGraph::resetGraph(float target_weight) {
  StaticJsonDocument<24> jsonDoc;
  jsonDoc["target_weight"] = target_weight;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  _ws.textAll(jsonString.c_str());
}

void WebSocketGraph::updateGraphData(float seconds, float weight) {
  // Create a JSON object
  StaticJsonDocument<64> jsonDoc;
  jsonDoc["seconds"] = seconds;
  jsonDoc["weight"] = weight;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  _ws.textAll(jsonString.c_str());
}
