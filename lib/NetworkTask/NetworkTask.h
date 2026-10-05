#pragma once

/**
 * Network task. Owns WiFiManager, AsyncWebServer, OTA and the single
 * multiplexed WebSocket at "/ws" (a JSON envelope with a "type"
 * discriminator). WiFi is provisioned through WiFiManager (AP
 * "Eureka setup"); mDNS advertises the device name. OTA has no password,
 * which is accepted for a trusted home LAN. "/" serves the SPA (webapp/)
 * from a mounted LittleFS partition; webapp/README.md describes how
 * that image is built.
 */

/** Creates and starts the Network task. */
void createNetworkTask();
