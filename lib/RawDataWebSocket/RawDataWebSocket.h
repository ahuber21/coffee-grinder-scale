#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

/**
 * WebSocket server for streaming raw ADC data during grinding operations.
 * Provides high-frequency raw scale readings for analysis and debugging.
 */
class RawDataWebSocket {
private:
    AsyncWebSocket* ws;
    unsigned long lastSendTime;
    unsigned long sendIntervalMs;

    // JSON document for efficient message formatting
    StaticJsonDocument<256> jsonDoc;

public:
    RawDataWebSocket();

    /**
     * Initialize the WebSocket server on the specified endpoint.
     * @param server AsyncWebServer instance to attach to
     * @param endpoint WebSocket endpoint path (default: "/RawDataWebSocket")
     * @param frequencyHz Desired logging frequency (20-50 Hz recommended)
     */
    void begin(AsyncWebServer& server, const char* endpoint = "/RawDataWebSocket", float frequencyHz = 25.0f);

    /**
     * Send raw ADC data if throttling interval has elapsed.
     * Only call this during grinding states for automatic rate limiting.
     * @param rawValue Raw ADC reading from scale (single sample, not averaged)
     * @param filteredValue Ring buffer filtered value in grams
     * @param timestamp Relative timestamp in milliseconds since grinding started
     * @param isStable Whether the raw reading is from a stable measurement
     */
    void sendRawData(int32_t rawValue, float filteredValue, unsigned long timestamp, bool isStable);

    /**
     * Send completion event to signal end of data collection.
     * Clients should use this to finalize their data processing/storage.
     */
    void sendComplete();

    /**
     * Get number of connected clients.
     * @return Number of active WebSocket connections
     */
    size_t getClientCount() const;
};