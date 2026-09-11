#include "DisplayTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"

namespace {

const char *modeName(DisplayMode m) {
  switch (m) {
    case DisplayMode::BOOT: return "BOOT";
    case DisplayMode::IDLE: return "IDLE";
    case DisplayMode::CONFIRM: return "CONFIRM";
    case DisplayMode::TARE: return "TARE";
    case DisplayMode::GRINDING: return "GRINDING";
    case DisplayMode::TOPUP: return "TOPUP";
    case DisplayMode::STOPPING: return "STOPPING";
    case DisplayMode::FINALIZE: return "FINALIZE";
    case DisplayMode::SCREENSAVER: return "SCREENSAVER";
    case DisplayMode::DEBUG: return "DEBUG";
    case DisplayMode::OTA_UPDATE: return "OTA_UPDATE";
  }
  return "?";
}

void displayTaskFn(void *) {
  // Real ST7735 init would happen here (SPI bus setup, Adafruit_ST7735
  // begin(), backlight on). Stubbed per the task brief -- the mailbox
  // drain/diffing discipline is the real part of this skeleton.
  xEventGroupSetBits(g_sys_events, kDisplayReadyBit);

  DisplayCommand last{};
  last.seq = 0;
  bool haveLast = false;

  const TickType_t frameFloor = pdMS_TO_TICKS(33);  // §6.4
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    DisplayCommand cmd;
    // Block up to the frame floor for a new command; this both rate-limits
    // needless redraw work and lets the task idle when nothing changed.
    if (xQueueReceive(g_display_mailbox, &cmd, frameFloor) == pdTRUE) {
      bool changed = !haveLast || cmd.seq != last.seq ||
                     cmd.mode != last.mode ||
                     cmd.current_grams != last.current_grams ||
                     cmd.target_grams != last.target_grams ||
                     cmd.connection_indicator_color !=
                         last.connection_indicator_color;
      if (changed) {
        // Real rendering (targeted fillRect per changed field, §3.2/AR-018)
        // goes here once the ST7735 driver is ported in. For now: log what
        // would be (re)drawn, applied uniformly to every mode -- there is no
        // per-mode special case at this layer, matching the design's intent.
        Serial.printf("[Display] mode=%s current=%.2fg target=%.2fg conn=%u\n",
                      modeName(cmd.mode), cmd.current_grams, cmd.target_grams,
                      cmd.connection_indicator_color);
      }
      last = cmd;
      haveLast = true;
    }

    vTaskDelayUntil(&lastFrame, frameFloor);
  }
}

}  // namespace

void createDisplayTask() {
  xTaskCreatePinnedToCore(displayTaskFn, "Display",
                           TaskConfig::kDisplayStackBytes, nullptr,
                           TaskConfig::kDisplayPriority, nullptr,
                           TaskConfig::kDisplayCore);
}
