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

/*
 * The three buttons are wired asymmetrically on this hardware, not
 * uniformly: LEFT/RIGHT are pulled up (idle HIGH, pressed pulls the
 * pin LOW), BACK is pulled down (idle LOW, pressed pulls it HIGH) --
 * confirmed against the original firmware's pinMode calls, and
 * directly confirmed live on this device (LEFT's debounce diagnostic
 * showed held_high=0 at verification time, i.e. it never reads HIGH
 * when pressed). Indexed by ButtonId: LEFT, RIGHT, BACK.
 */
constexpr uint8_t kActiveLevel[3] = {LOW, LOW, HIGH};

/** Identical ISR template for all three buttons: no global state touched, no settings read. */
template <ButtonId B, uint8_t Pin>
void IRAM_ATTR buttonIsr() {
  bool active = digitalRead(Pin) == kActiveLevel[static_cast<uint8_t>(B)];
  ButtonEdge e{B, active, millis()};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(g_button_edge_q, &e, &woken);
  portYIELD_FROM_ISR(woken);
}

/** Per-button candidate-press state, awaiting hold-time verification. */
struct DebounceState {
  bool pending = false;
  bool level = false;
  uint32_t edge_ms = 0;
};

DebounceState g_debounce[3];  ///< Indexed by ButtonId.

/// Last edge (press or release, any button) accepted past the debounce
/// gate below -- separate from DebounceState, which only tracks a
/// press candidate awaiting hold-time verification.
uint32_t g_last_edge_ms[3] = {0, 0, 0};
bool g_have_last_edge[3] = {false, false, false};

uint32_t g_min_hold_ms = 20;       ///< Overwritten from settings once loaded.
uint32_t g_button_debounce_ms = 150;  ///< Overwritten from settings once loaded.

/** Refreshes g_min_hold_ms/g_button_debounce_ms from the latest settings snapshot, if any. */
void applySettings() {
  SettingsSnapshot snap;
  if (xQueuePeek(g_settings_mailbox_input, &snap, 0) == pdTRUE) {
    g_min_hold_ms = snap.button_min_hold_ms;
    g_button_debounce_ms = snap.button_debounce_ms;
  }
}

/** Input task entry point: attaches the button ISRs, then debounces edges. */
void inputTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kSettingsLoadedBit, pdFALSE, pdTRUE,
                       portMAX_DELAY);
  applySettings();

  pinMode(BUTTON_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_BACK, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT),
                   buttonIsr<ButtonId::LEFT, BUTTON_LEFT>, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT),
                   buttonIsr<ButtonId::RIGHT, BUTTON_RIGHT>, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_BACK),
                   buttonIsr<ButtonId::BACK, BUTTON_BACK>, CHANGE);

  ButtonEdge edge;
  for (;;) {
    applySettings();

    // Drain every pending edge (queue depth 8, human-timescale bursts only).
    while (xQueueReceive(g_button_edge_q, &edge, 0) == pdTRUE) {
      uint8_t idx = static_cast<uint8_t>(edge.button);
      // Debounce gate: an edge arriving within button_debounce_ms of the
      // last accepted edge on this same button is contact bounce, not a
      // real transition -- dropped before it can touch the hold-time
      // candidate below. Separate from g_min_hold_ms, which verifies an
      // already-accepted press stays held, not whether the edge itself
      // was real.
      if (g_have_last_edge[idx] && edge.millis - g_last_edge_ms[idx] < g_button_debounce_ms) {
        continue;
      }
      g_have_last_edge[idx] = true;
      g_last_edge_ms[idx] = edge.millis;

      DebounceState &d = g_debounce[idx];
      if (edge.level) {
        // Candidate press: remember it, verify after the hold time.
        d.pending = true;
        d.level = true;
        d.edge_ms = edge.millis;
      } else {
        d.pending = false;
      }
    }

    // Every button, every state, the same hold-time verification -- no
    // per-state exception exists at this layer.
    uint32_t now = millis();
    for (uint8_t i = 0; i < 3; ++i) {
      DebounceState &d = g_debounce[i];
      if (d.pending && (now - d.edge_ms) >= g_min_hold_ms) {
        auto pin = i == 0 ? BUTTON_LEFT : (i == 1 ? BUTTON_RIGHT : BUTTON_BACK);
        bool still_active = digitalRead(pin) == kActiveLevel[i];
        // Reports every debounce decision, not just accepted presses,
        // so a wiring/polarity issue is visible over the WS log too.
        TelemetryEvent diag{};
        diag.type = TelemetryType::LOG_LINE;
        snprintf(diag.log_line, sizeof(diag.log_line),
                  "InputTask debounce id=%u pin=%d still_active=%d", i,
                  static_cast<int>(pin), still_active ? 1 : 0);
        xQueueSend(g_telemetry_q, &diag, 0);
        if (still_active) {
          ButtonPress press{static_cast<ButtonId>(i), now};
          xQueueSend(g_button_press_q, &press, 0);
        }
        d.pending = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void createInputTask() {
  xTaskCreatePinnedToCore(inputTaskFn, "Input", TaskConfig::kInputStackBytes,
                           nullptr, TaskConfig::kInputPriority, nullptr,
                           TaskConfig::kInputCore);
}
