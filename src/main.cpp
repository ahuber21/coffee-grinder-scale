/**
 * FreeRTOS task/queue skeleton: 7 independent tasks communicating over
 * queues and mailboxes. This file is deliberately thin: its only job
 * is to create the shared queues/mailboxes/event group and start every
 * task. All real behavior lives in each task's own lib/<Name>Task
 * module.
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

/** Arduino entry point: wires up shared state, then starts all 7 tasks. */
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[main] coffee-grinder-scale RTOS skeleton starting");

  // Must run before any task that touches a queue/mailbox/event-group
  // bit is created.
  initQueuesAndEvents();

  // Starting Settings task first isn't required (every task waits on
  // its own readiness bits before touching shared state), but gives its
  // NVS load a head start before anything blocks on it being ready.
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
  // Arduino's own loopTask (which called setup()/loop()) has no further
  // work of its own -- delete it rather than spin an idle loop.
  vTaskDelete(nullptr);
}
