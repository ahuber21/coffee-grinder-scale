#include "InputTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"

namespace {

/** Identical ISR template for all three buttons: no global state touched, no settings read. */
template <ButtonId B, uint8_t Pin>
void IRAM_ATTR buttonIsr() {
  ButtonEdge e{B, digitalRead(Pin) == HIGH, millis()};
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

uint32_t g_min_hold_ms = 20;  ///< Overwritten from settings once loaded.

/** Refreshes g_min_hold_ms from the latest settings snapshot, if any. */
void applySettings() {
  SettingsSnapshot snap;
  if (xQueuePeek(g_settings_mailbox_input, &snap, 0) == pdTRUE) {
    g_min_hold_ms = snap.button_min_hold_ms;
  }
}

/** Input task entry point: attaches the button ISRs, then debounces edges. */
void inputTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kSettingsLoadedBit, pdFALSE, pdTRUE,
                       portMAX_DELAY);
  applySettings();

  pinMode(BUTTON_LEFT, INPUT);
  pinMode(BUTTON_RIGHT, INPUT);
  pinMode(BUTTON_BACK, INPUT);
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
      DebounceState &d = g_debounce[static_cast<uint8_t>(edge.button)];
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
        if (digitalRead(pin) == HIGH) {
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
