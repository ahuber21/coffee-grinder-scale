#include "InputTask.h"

#include <Arduino.h>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"

namespace {

constexpr uint8_t kButtonCount = 3;

/// GPIO per button, indexed by ButtonId: LEFT, RIGHT, BACK.
constexpr uint8_t kButtonPins[kButtonCount] = {BUTTON_LEFT, BUTTON_RIGHT, BUTTON_BACK};

/*
 * The wiring is asymmetric: LEFT and RIGHT are pulled up (a press pulls
 * the pin LOW) while BACK is pulled down (a press pulls it HIGH).
 * Indexed by ButtonId.
 */
constexpr uint8_t kActiveLevel[kButtonCount] = {LOW, LOW, HIGH};

/** Identical ISR template for all three buttons; it only enqueues the edge. */
template <ButtonId B, uint8_t Pin>
void IRAM_ATTR buttonIsr() {
  bool active = digitalRead(Pin) == kActiveLevel[static_cast<uint8_t>(B)];
  ButtonEdge e{B, active, millis()};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(g_button_edge_q, &e, &woken);
  portYIELD_FROM_ISR(woken);
}

/** Per-button debounce state. */
struct DebounceState {
  bool pending = false;  ///< A press candidate is awaiting hold-time verification.
  uint32_t edge_ms = 0;  ///< When that candidate press began.
  bool have_last_edge = false;
  uint32_t last_edge_ms = 0;  ///< Last edge, press or release, accepted past the debounce gate.
};

DebounceState g_debounce[kButtonCount];

uint32_t g_min_hold_ms = 20;          ///< Overwritten from settings once loaded.
uint32_t g_button_debounce_ms = 150;  ///< Overwritten from settings once loaded.

/** Refreshes the debounce timings from the latest settings snapshot, if any. */
void applySettings() {
  SettingsSnapshot snap;
  if (xQueuePeek(g_settings_mailbox_input, &snap, 0) == pdTRUE) {
    g_min_hold_ms = snap.button_min_hold_ms;
    g_button_debounce_ms = snap.button_debounce_ms;
  }
}

/**
 * Handles one raw edge. An edge within button_debounce_ms of the last
 * accepted one on the same button is contact bounce and is dropped. A
 * press edge starts a candidate, verified later against button_min_hold_ms.
 */
void handleEdge(const ButtonEdge &edge) {
  DebounceState &d = g_debounce[static_cast<uint8_t>(edge.button)];
  if (d.have_last_edge && edge.millis - d.last_edge_ms < g_button_debounce_ms) {
    return;
  }
  d.have_last_edge = true;
  d.last_edge_ms = edge.millis;

  d.pending = edge.level;
  if (edge.level) {
    d.edge_ms = edge.millis;
  }
}

/** Confirms each press candidate that has been held long enough and is still active. */
void confirmHeldPresses() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    DebounceState &d = g_debounce[i];
    if (!d.pending || now - d.edge_ms < g_min_hold_ms) {
      continue;
    }
    d.pending = false;
    if (digitalRead(kButtonPins[i]) == kActiveLevel[i]) {
      ButtonPress press{static_cast<ButtonId>(i), now};
      xQueueSend(g_button_press_q, &press, 0);
    } else {
      TelemetryEvent diag{};
      diag.type = TelemetryType::LOG_LINE;
      snprintf(diag.log_line, sizeof(diag.log_line), "button %u released before the hold time", i);
      xQueueSend(g_telemetry_q, &diag, 0);
    }
  }
}

/** Input task entry point: attaches the button ISRs, then debounces edges. */
void inputTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kSettingsLoadedBit, pdFALSE, pdTRUE, portMAX_DELAY);
  applySettings();

  pinMode(BUTTON_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_BACK, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT), buttonIsr<ButtonId::LEFT, BUTTON_LEFT>, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT), buttonIsr<ButtonId::RIGHT, BUTTON_RIGHT>, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_BACK), buttonIsr<ButtonId::BACK, BUTTON_BACK>, CHANGE);

  ButtonEdge edge;
  for (;;) {
    applySettings();
    while (xQueueReceive(g_button_edge_q, &edge, 0) == pdTRUE) {
      handleEdge(edge);
    }
    confirmHeldPresses();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void createInputTask() {
  xTaskCreatePinnedToCore(inputTaskFn, "Input", TaskConfig::kInputStackBytes, nullptr,
                          TaskConfig::kInputPriority, nullptr, TaskConfig::kInputCore);
}
