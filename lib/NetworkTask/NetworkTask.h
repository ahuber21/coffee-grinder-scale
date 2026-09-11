#pragma once

// Task #6 -- Network. Per rtos-architecture.md §7: owns WiFiManager,
// AsyncWebServer, the D4 consolidated WebSocket, and OTA.
//
// Real in this pass: WiFi provisioning (WiFiManager, AP name "Eureka setup"
// -- reused from the pre-rewrite src/main.cpp's setupWifi()), mDNS
// (eureka.local, D5), ArduinoOTA wired to the D12 refuse-before-start gate,
// an AsyncWebServer instance, and the one multiplexed WebSocket at "/ws"
// (connection handling, a JSON envelope with a "type" discriminator,
// AR-013/AR-014's cleanupClients()/connection-cap fixes). "/" now serves
// the real SPA (webapp/, D4/D19) from a mounted LittleFS partition --
// see webapp/README.md for the build step that populates it.
//
// Still stubbed: nothing NVS-shaped lives here (Settings task owns that);
// most TelemetryEvent/DisplayCommand *consumers* of the data this task
// pushes out don't have a frontend to render it yet, but the data itself is
// real (Dosing -> Telemetry -> g_ws_broadcast_q -> here -> ws.textAll()).

void createNetworkTask();

// D12: Network task's OTA-begin handler calls this before accepting a
// flash -- true means "safe to proceed", false means "grind in progress,
// refuse the OTA start". Exposed standalone so it can be exercised without a
// real ArduinoOTA/AsyncWebServer instance.
bool otaSafeToStart();
