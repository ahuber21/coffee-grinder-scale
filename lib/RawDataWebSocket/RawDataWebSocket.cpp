#include "RawDataWebSocket.h"

RawDataWebSocket::RawDataWebSocket()
    : ws(nullptr), lastSendTime(0), sendIntervalMs(40) {
}

void RawDataWebSocket::begin(AsyncWebServer& server, const char* endpoint, float frequencyHz) {
    if (frequencyHz <= 0) {
        frequencyHz = 25.0f; // Default to 25 Hz
    }

    sendIntervalMs = (unsigned long)(1000.0f / frequencyHz);
    lastSendTime = 0; // Force immediate send on first call

    ws = new AsyncWebSocket(endpoint);

    ws->onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
        switch (type) {
            case WS_EVT_CONNECT:
                // Limit to single connection for security and resource management
                if (server->count() > 1) {
                    client->close(1008, "Only one connection allowed");
                }
                break;
            case WS_EVT_DISCONNECT:
                // Client disconnected - no action needed
                break;
            case WS_EVT_DATA:
                // Handle incoming data if needed (currently not implemented)
                break;
            case WS_EVT_PONG:
            case WS_EVT_ERROR:
                // Handle pong/error events if needed
                break;
        }
    });

    server.addHandler(ws);
}

void RawDataWebSocket::sendRawData(int32_t rawValue, float filteredValue, unsigned long timestamp, bool isStable) {
    if (!ws || getClientCount() == 0) {
        return;
    }

    unsigned long currentTime = millis();
    if (currentTime - lastSendTime < sendIntervalMs) {
        return; // Throttling - not time to send yet
    }

    // Clear and populate JSON document
    jsonDoc.clear();
    jsonDoc["runtime_ms"] = timestamp;
    jsonDoc["raw"] = rawValue;
    jsonDoc["filtered"] = filteredValue;
    jsonDoc["stable"] = isStable;

    // Serialize and send to all clients
    String message;
    serializeJson(jsonDoc, message);
    ws->textAll(message);

    lastSendTime = currentTime;
}

void RawDataWebSocket::sendComplete() {
    if (!ws || getClientCount() == 0) {
        return;
    }

    // Send completion event
    jsonDoc.clear();
    jsonDoc["event"] = "complete";

    String message;
    serializeJson(jsonDoc, message);
    ws->textAll(message);
}

size_t RawDataWebSocket::getClientCount() const {
    return ws ? ws->count() : 0;
}