/**
 * Firmware entry point. The firmware is seven independent FreeRTOS tasks
 * that communicate over queues and mailboxes; this file only creates the
 * shared queues and event group and starts each task, and all behavior
 * lives in the lib/<Name>Task modules.
 */

#include <Arduino.h>

#include "DisplayTask.h"
#include "DosingTask.h"
#include "InputTask.h"
#include "NetworkTask.h"
#include "Queues.h"
#include "ScaleTask.h"
#include "SettingsTask.h"
#include "TelemetryTask.h"

/** Arduino entry point: wires up shared state, then starts all seven tasks. */
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[main] coffee-grinder-scale starting");

  // The queues must exist before any task that uses them is created.
  initQueuesAndEvents();

  // Settings goes first so its NVS load gets a head start; every other
  // task waits on its own readiness bits regardless.
  createSettingsTask();
  createScaleTask();
  createInputTask();
  createDisplayTask();
  createDosingTask();
  createNetworkTask();
  createTelemetryTask();

  Serial.println("[main] all 7 tasks created");
}

/** Unused: all work happens in the tasks setup() created. */
void loop() {
  // Delete Arduino's own loop task rather than spin an idle loop.
  vTaskDelete(nullptr);
}
