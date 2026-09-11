#pragma once

// Task #6 -- Network. Per rtos-architecture.md §7: owns WiFiManager,
// AsyncWebServer, the future consolidated WebSocket, and OTA. Per the task
// brief, WiFiManager/AsyncWebServer/ArduinoOTA wiring is out of scope for
// this skeleton -- what's real here is the task itself, the queues it reads
// from/writes to (ws_broadcast_q inbound, DoseRequest/SettingsWriteRequest
// outbound -- neither used yet since there's no HTTP/WS surface to drive
// them), and the D12 OTA-refuse-during-grind gate against the shared status
// event group.

void createNetworkTask();

// D12: Network task's OTA-begin handler should call this before accepting a
// flash -- true means "safe to proceed", false means "grind in progress,
// refuse the OTA start". Exposed standalone (not buried in the task body) so
// it can be exercised without a real ArduinoOTA/AsyncWebServer instance,
// which this skeleton doesn't wire up yet.
bool otaSafeToStart();
