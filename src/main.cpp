// FreeRTOS task/queue skeleton for the coffee-grinder-scale rewrite.
// Replaces the old single-loop()/switch(state) firmware (see git history on
// `main` for that version) with the 7-task architecture designed in
// .agent/design/rtos-architecture.md. See that document for the full
// rationale; this file is deliberately thin -- its only job is to create
// the queues/mailboxes/event group (§3/§4/§8) and then start every task
// with the priority/core/stack-size table from §1/§6.7. All real behavior
// lives in each task's own lib/<Name>Task module.

#include <Arduino.h>

#include "DisplayTask.h"
#include "DosingTask.h"
#include "InputTask.h"
#include "NetworkTask.h"
#include "Queues.h"
#include "ScaleTask.h"
#include "SettingsTask.h"
#include "TelemetryTask.h"

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[main] coffee-grinder-scale RTOS skeleton starting");

  // Must run before any task that touches a queue/mailbox/event-group bit
  // is created (§3/§4/§8).
  initQueuesAndEvents();

  // Settings task first isn't required by construction (every task waits on
  // the appropriate event-group bits before touching shared state), but
  // starting it early means its NVS-load stub has a head start before
  // anything blocks on SETTINGS_LOADED.
  createSettingsTask();
  createScaleTask();
  createInputTask();
  createDisplayTask();
  createDosingTask();
  createNetworkTask();
  createTelemetryTask();

  Serial.println("[main] all 7 tasks created");
}

void loop() {
  // Everything happens in the tasks created above; Arduino's own loopTask
  // (which called setup()/loop()) has no further work of its own. Delete it
  // rather than spin an idle loop that never yields anything useful.
  vTaskDelete(nullptr);
}
