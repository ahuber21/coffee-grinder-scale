#pragma once

/**
 * Network task. Owns WiFiManager, AsyncWebServer, OTA, and the single
 * multiplexed WebSocket at "/ws" (connection handling, a JSON envelope
 * with a "type" discriminator). WiFi provisioning uses WiFiManager
 * with AP name "Eureka setup"; mDNS advertises the device as
 * eureka.local, with no OTA password (accepted risk, trusted home LAN
 * only). "/" serves the SPA (webapp/) from a mounted LittleFS
 * partition -- see webapp/README.md for the build step that populates
 * it.
 */

/** Creates and starts the Network task. */
void createNetworkTask();

/**
 * True if it is safe to begin an OTA flash right now, false if a grind
 * is in progress and the OTA start must be refused. Network task's
 * OTA-begin handler calls this before accepting a flash. Exposed
 * standalone so it can be exercised without a real
 * ArduinoOTA/AsyncWebServer instance.
 */
bool otaSafeToStart();
