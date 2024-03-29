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
        let targetDataset;
        let weightDataset;
        let targetWeight;

        function createChart() {
            // recreate graphCanvas to reset all contents
            const existingCanvas = document.getElementById('graphCanvas');
            existingCanvas.parentNode.removeChild(existingCanvas);
            const newCanvas = document.createElement('canvas');
            newCanvas.id = 'graphCanvas';
            newCanvas.width = 800;
            newCanvas.height = 400;
            document.body.appendChild(newCanvas);

            const ctx = document.getElementById('graphCanvas');

            targetDataset = {
                label: 'Target',
                borderColor: '#a83632',
                borderWidth: 4,
                radius: 0,
                fill: false,
                data: [],
            };

            weightDataset = {
                label: 'Weight',
                backgroundColor: '#ffcc00',
                borderColor: '#ffcc00',
                borderWidth: 2,
                fill: false,
                data: [],
            };

            weightChart = new Chart(ctx, {
                type: 'line',
                data: {
                    datasets: [targetDataset, weightDataset],
                },
                options: {
                    responsive: true,
                    animation: false,
                    scales: {
                        x: {
                            type: 'linear',
                            position: 'bottom',
                            title: {
                                display: true,
                                text: 'Time [s]',
                                color: 'white',
                            },
                            ticks: {
                                color: 'white',
                            },
                        },
                        y: {
                            type: 'linear',
                            position: 'left',
                            title: {
                                display: true,
                                text: 'Weight [g]',
                                color: 'white',
                            },
                            ticks: {
                                color: 'white',
                            },
                        },
                    },
                    plugins: {
                        legend: {
                            display: false,
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
            if (data.target_weight) {
                targetWeight = data.target_weight
                createChart();
            } else if (data.finalize) {
                weightDataset.backgroundColor = '#529641';
                weightDataset.borderColor = '#529641';
                weightChart.update();
            } else {
                targetDataset.data.push({ x: data.seconds, y: targetWeight });
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

WebSocketGraph::WebSocketGraph()
    : _ws("/GraphWebSocket"), _server(nullptr), _lastWeightValue(-1.0f),
      _lastSecondsValue(-1.0f) {}

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

  _lastWeightValue = -1.0f;
  _lastSecondsValue = -1.0f;
}

void WebSocketGraph::updateGraphData(float seconds, float weight) {
  if ((seconds - _lastSecondsValue < 0.2) && (weight - _lastWeightValue < 0.1))
    return;

  // cache to limit update rate
  _lastWeightValue = weight;
  _lastSecondsValue = seconds;

  StaticJsonDocument<64> jsonDoc;
  char s[8], w[8];
  sprintf(s, "%1.2f", seconds);
  sprintf(w, "%1.2f", weight);
  jsonDoc["seconds"] = s;
  jsonDoc["weight"] = w;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  _ws.textAll(jsonString.c_str());
}

void WebSocketGraph::finalizeGraph() {
  StaticJsonDocument<20> jsonDoc;
  jsonDoc["finalize"] = true;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  _ws.textAll(jsonString.c_str());
}