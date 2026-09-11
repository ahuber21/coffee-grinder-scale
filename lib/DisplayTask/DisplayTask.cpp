// Task #4 -- Display. Real ST7735 rendering, per .agent/design/
// rtos-architecture.md §3.2/§6.4 and D3 ("push the existing 80x160 ST7735 to
// its limit -- smooth, flicker-free, high-framerate, no resolution upgrade
// available").
//
// Redraw strategy (D10: question, don't transliterate):
//
// The old lib/Display/ module (kept in the tree for reference, AR-026)
// already did the right thing at the *within-a-screen* level: every layout
// function diffs the new value against a handful of static "what did we
// draw last time" variables and issues `fillRect` only over the pixels that
// actually need to change (e.g. only the shrinking digits of the integer
// part, not the whole number). That discipline is kept and ported here
// essentially unchanged -- it's exactly the "targeted partial/dirty-rect
// redraw" this hardware needs to look flicker-free, and rewriting it from
// scratch would just be re-deriving the same pixel math with more risk.
//
// What *does* change from the old code:
//
//   - One mode-dispatch loop instead of a `switch(state)` spread across
//     main.cpp, driven by the single `DisplayCommand` mailbox (§3.2) --
//     every mode goes through the same "did the mode change -> full clear;
//     otherwise -> per-field targeted redraw" path. No per-mode special
//     case at that top level (this is what closes AR-018: the old
//     CONFIRM layout was the one holdout that did a full `fillScreen` on
//     every value change instead of a targeted clear; it's now a small
//     `fillRect` over just the digits/title, like every other layout).
//   - The connection indicator is redrawn with a symmetric erase: if its
//     color changes (including changing *to* "none", i.e. 0), the old dot
//     is erased before anything else happens. The old code only ever drew
//     the dot when the color was non-zero and never erased it otherwise,
//     so a client disconnecting left a stale dot on screen forever (the
//     exact bug AR-012 names). Because `DisplayCommand` always carries
//     `connection_indicator_color` as a first-class field, this task's one
//     diffing pass naturally covers it -- there's no separate code path
//     that could "forget" to erase it.
//   - The old per-widget millis()-based FPS limiter (`m_fps`,
//     `m_lastDisplayRefreshMillis`) is dropped. It's redundant here: the
//     mailbox is a depth-1 overwrite box (§3.2, "latest wins") and this
//     task only ever drains it inside its own ~33ms `vTaskDelayUntil`
//     floor, so the frame rate is already capped once, at the task level
//     (§6.4). A second, per-widget cap inside the draw functions bought
//     nothing but the risk of a value update being silently skipped inside
//     its own rate window (a real edge case in the old code).
//   - The old backlight on/off (`wakeUp()`/`shutDown()`) machinery is
//     dropped -- grep of the old main.cpp shows `shutDown()` was never
//     called anywhere; the backlight was always just "on". Carrying forward
//     unused state-toggling machinery didn't seem worth it (D10).
//
// Per-mode color fields (`current_color`/`target_color`/`time_color`) are
// part of `DisplayCommand` so the producer can drive them, but Dosing task's
// `sendDisplayCommand()` (lib/DosingTask/DosingTask.cpp) doesn't populate
// them yet -- they arrive as 0 (black), which would render invisible text.
// This task treats 0 as "unset" and falls back to the same per-mode color
// scheme the old main.cpp hardcoded at each `displayGrindingLayout()` call
// site (white while grinding, cyan while topping up/settling, green at
// finalize) -- see `defaultColorFor()`. A non-zero value from the producer
// always wins. Worth revisiting once Dosing task is extended to set these
// explicitly; noted for STATUS/ARs, not fixed by touching DosingTask.cpp
// here (out of this task's scope).

#include "DisplayTask.h"

#include <Adafruit_ST7735.h>
#include <Arduino.h>
#include <SPI.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"

namespace {

constexpr int16_t kW = 80;
constexpr int16_t kH = 160;

SPIClass g_spi(HSPI);
Adafruit_ST7735 g_tft(&g_spi, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RESET_PIN);

// ---------------------------------------------------------------------------
// Panel bring-up
// ---------------------------------------------------------------------------

// ledcSetup(1, 100, 8) below configures an 8-bit duty register (0-255); the
// old code's equivalent (Display::setBrightness) scaled from a 32-bit
// 0xFFFFFFFF constant instead of the actual 8-bit resolution, so the duty
// value it wrote was effectively truncated/wrapped by hardware rather than
// meaning what its own math implied -- harmless there since it was only ever
// called with 0 or 1 (an on/off toggle, never a real percentage), but worth
// doing correctly here since backlight is called with a real percent.
void backlightPercent(uint8_t percent) {
  uint32_t duty = (static_cast<uint32_t>(percent) * 255u) / 100u;
  ledcWrite(1, duty);
}

void panelBegin() {
  g_spi.begin(DISPLAY_SCK_PIN, DISPLAY_MISO_PIN, DISPLAY_MOSI_PIN, DISPLAY_SS_PIN);
  g_tft.initR(INITR_MINI160x80);
  g_tft.setRotation(0);
  g_tft.invertDisplay(false);
  ledcAttachPin(DISPLAY_BACKLIGHT_PIN, 1);
  ledcSetup(1, 100, 8);
  backlightPercent(100);
  g_tft.fillScreen(ST7735_BLACK);
}

// ---------------------------------------------------------------------------
// Connection indicator -- AR-012 fix: symmetric erase, not just conditional
// draw. Small bottom-left dot; 0 = none.
// ---------------------------------------------------------------------------

uint16_t g_lastConnColor = 0;

void drawConnectionIndicator(uint16_t color) {
  if (color == g_lastConnColor) {
    return;
  }
  constexpr int16_t x = 1;
  constexpr int16_t y = kH - 4;
  constexpr int16_t r = 3;
  if (g_lastConnColor != 0) {
    g_tft.fillRect(x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2, ST7735_BLACK);
  }
  if (color != 0) {
    g_tft.fillCircle(x, y, r, color);
  }
  g_lastConnColor = color;
}

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------

// Rounds tiny +/-0.0x readings to a clean 0.0 so the display never flickers
// between "0.0" and "-0.0" from load-cell noise around zero (kept from the
// old code's identical guard in displayGrindingLayout/displayIdleLayout).
float cleanZero(float grams) { return (grams > -0.05f && grams < 0.05f) ? 0.0f : grams; }

void splitDecigrams(float grams, char *intStr, size_t intCap, char *decStr,
                     size_t decCap, bool *isNegative) {
  long totalDecigrams = lroundf(fabsf(grams) * 10.0f);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;
  *isNegative = grams < -0.05f;
  snprintf(intStr, intCap, "%d", intPart);
  snprintf(decStr, decCap, "%d", decPart);
}

// ---------------------------------------------------------------------------
// Generic centered single line of text -- used for BOOT ("Eureka") and TARE
// ("T"). Both are entered via a mode change (which already triggers a full
// screen clear at the call site), so a single shared "last drawn" cache is
// safe -- it's always invalidated by `force` before either mode's first
// frame is drawn.
// ---------------------------------------------------------------------------

struct { char text[16] = ""; uint8_t size = 0; uint16_t color = 0; } g_centered;

void drawCentered(const char *text, uint8_t size, uint16_t color, bool force) {
  if (!force && strncmp(text, g_centered.text, sizeof(g_centered.text)) == 0 &&
      size == g_centered.size && color == g_centered.color) {
    return;
  }
  int16_t x, y;
  uint16_t w, h;
  g_tft.setTextSize(size);
  g_tft.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  g_tft.fillRect(0, kH / 2 - h / 2 - 2, kW, h + 4, ST7735_BLACK);
  g_tft.setTextColor(color, ST7735_BLACK);
  g_tft.setCursor(kW / 2 - w / 2, kH / 2 - h / 2);
  g_tft.print(text);

  strncpy(g_centered.text, text, sizeof(g_centered.text) - 1);
  g_centered.text[sizeof(g_centered.text) - 1] = '\0';
  g_centered.size = size;
  g_centered.color = color;
}

// ---------------------------------------------------------------------------
// IDLE layout: big centered weight readout, dot anchored at x=60 (ported
// from Display::displayIdleLayout).
// ---------------------------------------------------------------------------

struct {
  char text[16] = "";
  int16_t intStartX = -1;
  bool isNegative = false;
  bool wasMax = false;
} g_idle;

void drawIdleLayout(float currentGrams, uint16_t connColor, bool force) {
  if (force) {
    g_idle = {};
  }

  constexpr int16_t currentY = (kH - 32) / 2;

  if (fabsf(currentGrams) > 99.95f) {
    if (!force && strcmp(g_idle.text, "MAX") == 0) {
      drawConnectionIndicator(connColor);
      return;
    }
    g_tft.fillRect(0, currentY, kW, 32, ST7735_BLACK);
    g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
    g_tft.setTextSize(3);
    int16_t x, y;
    uint16_t w, h;
    g_tft.getTextBounds("MAX", 0, 0, &x, &y, &w, &h);
    g_tft.setCursor((kW - w) / 2, (kH - h) / 2);
    g_tft.print("MAX");
    strncpy(g_idle.text, "MAX", sizeof(g_idle.text));
    g_idle.intStartX = -1;
    g_idle.wasMax = true;
    drawConnectionIndicator(connColor);
    return;
  }

  float displayGrams = cleanZero(currentGrams);
  char text[16];
  snprintf(text, sizeof(text), "%5.1f", displayGrams);

  if (force || strcmp(text, g_idle.text) != 0) {
    char intStr[10];
    char decStr[5];
    bool isNegative;
    splitDecigrams(displayGrams, intStr, sizeof(intStr), decStr, sizeof(decStr),
                    &isNegative);

    constexpr uint8_t intSize = 4;
    constexpr uint8_t decSize = 2;
    constexpr uint8_t minusSize = 2;
    constexpr uint8_t dotSize = 1;

    int16_t intWidth = strlen(intStr) * 6 * intSize;
    int16_t intStartX = 60 - intWidth;

    if (g_idle.wasMax) {
      g_tft.fillRect(0, currentY, kW, 32, ST7735_BLACK);
    } else if (g_idle.intStartX != -1) {
      if (intStartX > g_idle.intStartX) {
        g_tft.fillRect(g_idle.intStartX, currentY, intStartX - g_idle.intStartX, 32,
                       ST7735_BLACK);
      }
      if (g_idle.isNegative && !isNegative) {
        g_tft.fillRect(0, currentY + 8, 6 * minusSize, 8 * minusSize, ST7735_BLACK);
      }
    }

    g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);

    if (isNegative) {
      g_tft.setCursor(0, currentY + 8);
      g_tft.setTextSize(minusSize);
      g_tft.print("-");
    }

    g_tft.setCursor(intStartX, currentY);
    g_tft.setTextSize(intSize);
    g_tft.print(intStr);

    g_tft.setCursor(60, currentY + 21);
    g_tft.setTextSize(dotSize);
    g_tft.print(".");

    g_tft.setCursor(60 + 6 * dotSize, currentY + 14);
    g_tft.setTextSize(decSize);
    g_tft.print(decStr);

    strncpy(g_idle.text, text, sizeof(g_idle.text) - 1);
    g_idle.text[sizeof(g_idle.text) - 1] = '\0';
    g_idle.intStartX = intStartX;
    g_idle.isNegative = isNegative;
    g_idle.wasMax = false;
  }

  drawConnectionIndicator(connColor);
}

// ---------------------------------------------------------------------------
// GRINDING/TOPUP/STOPPING/FINALIZE layout: current weight (top), target
// below, elapsed time at the bottom. Shared by every mode in this family --
// they only differ in the color they're drawn with (ported from
// Display::displayGrindingLayout).
// ---------------------------------------------------------------------------

struct {
  char currentStr[16] = "";
  char targetStr[16] = "";
  char timeStr[16] = "";
  uint16_t currentColor = 0xFFFF;
  uint16_t targetColor = 0xFFFF;
  uint16_t timeColor = 0xFFFF;
  int16_t layoutX = -1;
  int16_t layoutWidth = -1;
} g_grind;

void drawGrindingBlock(float currentGrams, float targetGrams, float seconds,
                       uint16_t currentColor, uint16_t targetColor,
                       uint16_t timeColor, uint16_t connColor, bool force) {
  if (force) {
    g_grind = {};
  }

  float displayGrams = cleanZero(currentGrams);

  char currentStr[16];
  char targetStr[16];
  char timeStr[16];
  snprintf(currentStr, sizeof(currentStr), "%4.1f", displayGrams);
  snprintf(targetStr, sizeof(targetStr), "/%4.1f", targetGrams);
  snprintf(timeStr, sizeof(timeStr), "%4.1fs", seconds);

  const bool sameCurrent = strcmp(currentStr, g_grind.currentStr) == 0 &&
                           currentColor == g_grind.currentColor;
  const bool sameTarget = strcmp(targetStr, g_grind.targetStr) == 0 &&
                          targetColor == g_grind.targetColor;
  const bool sameTime = strcmp(timeStr, g_grind.timeStr) == 0 &&
                        timeColor == g_grind.timeColor;

  if (!force && sameCurrent && sameTarget && sameTime) {
    drawConnectionIndicator(connColor);
    return;
  }

  constexpr uint8_t intSize = 4;
  constexpr uint8_t decSize = 2;
  constexpr uint8_t minusSize = 2;
  constexpr uint8_t dotSize = 2;
  constexpr int16_t currentY = 38;

  if (!sameCurrent) {
    char intStr[10];
    char decStr[5];
    bool isNegative;
    splitDecigrams(displayGrams, intStr, sizeof(intStr), decStr, sizeof(decStr),
                    &isNegative);

    int16_t intWidth = strlen(intStr) * 6 * intSize;
    int16_t decWidth = strlen(decStr) * 6 * decSize;
    int16_t minusWidth = isNegative ? (6 * minusSize) : 0;
    int16_t dotWidth = 6 * dotSize;
    int16_t totalWidth = minusWidth + intWidth + dotWidth + decWidth;
    int16_t currentX = (kW - totalWidth) / 2;

    if (currentX != g_grind.layoutX || totalWidth != g_grind.layoutWidth) {
      g_tft.fillRect(0, currentY, kW, 32, ST7735_BLACK);
    }
    g_grind.layoutX = currentX;
    g_grind.layoutWidth = totalWidth;

    g_tft.setTextColor(currentColor, ST7735_BLACK);

    if (isNegative) {
      g_tft.setCursor(currentX, currentY + 8);
      g_tft.setTextSize(minusSize);
      g_tft.print("-");
      currentX += minusWidth;
    }

    g_tft.setCursor(currentX, currentY);
    g_tft.setTextSize(intSize);
    g_tft.print(intStr);
    currentX += intWidth;

    g_tft.setCursor(currentX, currentY + 16);
    g_tft.setTextSize(dotSize);
    g_tft.print(".");
    currentX += dotWidth;

    g_tft.setCursor(currentX, currentY + 16);
    g_tft.setTextSize(decSize);
    g_tft.print(decStr);
  }

  // target, below current
  constexpr uint8_t targetSize = 2;
  int16_t x, y;
  uint16_t w, h;
  g_tft.setTextSize(targetSize);
  g_tft.getTextBounds(targetStr, 0, 0, &x, &y, &w, &h);
  const int16_t targetY = currentY + 32 + 10;

  if (!sameTarget) {
    g_tft.fillRect(0, targetY, kW, h, ST7735_BLACK);
    g_tft.setCursor((kW - w) / 2, targetY);
    g_tft.setTextColor(targetColor, ST7735_BLACK);
    g_tft.print(targetStr);
  }

  // time, at the bottom
  constexpr uint8_t timeSize = 2;
  g_tft.setTextSize(timeSize);
  g_tft.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);
  const int16_t timeY = targetY + h + 20;

  if (!sameTime) {
    g_tft.fillRect(0, timeY, kW, h, ST7735_BLACK);
    g_tft.setCursor((kW - w) / 2, timeY);
    g_tft.setTextColor(timeColor, ST7735_BLACK);
    g_tft.print(timeStr);
  }

  strncpy(g_grind.currentStr, currentStr, sizeof(g_grind.currentStr) - 1);
  g_grind.currentStr[sizeof(g_grind.currentStr) - 1] = '\0';
  strncpy(g_grind.targetStr, targetStr, sizeof(g_grind.targetStr) - 1);
  g_grind.targetStr[sizeof(g_grind.targetStr) - 1] = '\0';
  strncpy(g_grind.timeStr, timeStr, sizeof(g_grind.timeStr) - 1);
  g_grind.timeStr[sizeof(g_grind.timeStr) - 1] = '\0';
  g_grind.currentColor = currentColor;
  g_grind.targetColor = targetColor;
  g_grind.timeColor = timeColor;

  drawConnectionIndicator(connColor);
}

// Old main.cpp's per-mode color scheme (§ file header) -- used whenever the
// producer leaves a color field at 0 (unset).
void defaultColorsFor(DisplayMode mode, uint16_t *current, uint16_t *target,
                      uint16_t *time) {
  *target = ST7735_WHITE;
  *time = ST7735_WHITE;
  switch (mode) {
    case DisplayMode::TOPUP:
    case DisplayMode::STOPPING:
      *current = ST7735_CYAN;
      break;
    case DisplayMode::FINALIZE:
      *current = ST7735_GREEN;
      break;
    default:
      *current = ST7735_WHITE;
      break;
  }
}

// ---------------------------------------------------------------------------
// CONFIRM layout: large target weight + "OK?" prompt. AR-018 fix: targeted
// fillRect over just the two text regions instead of the old code's
// full-screen fillScreen on every redraw.
// ---------------------------------------------------------------------------

struct { float targetGrams = -1.0f; } g_confirm;

void drawConfirmLayout(float targetGrams, bool force) {
  if (!force && g_confirm.targetGrams == targetGrams) {
    return;
  }
  g_confirm.targetGrams = targetGrams;

  char intStr[10];
  char decStr[5];
  bool isNegative;
  splitDecigrams(targetGrams, intStr, sizeof(intStr), decStr, sizeof(decStr),
                 &isNegative);

  constexpr uint8_t intSize = 4;
  constexpr uint8_t decSize = 2;
  constexpr uint8_t dotSize = 2;

  int16_t intWidth = strlen(intStr) * 6 * intSize;
  int16_t decWidth = strlen(decStr) * 6 * decSize;
  int16_t dotWidth = 6 * dotSize;
  int16_t totalWidth = intWidth + dotWidth + decWidth;

  constexpr int16_t weightY = 38;
  int16_t weightX = (kW - totalWidth) / 2;

  static const char *title = "OK?";
  constexpr uint8_t titleSize = 3;
  int16_t x, y;
  uint16_t w, h;
  g_tft.setTextSize(titleSize);
  g_tft.getTextBounds(title, 0, 0, &x, &y, &w, &h);
  int16_t titleX = (kW - w) / 2;
  int16_t titleY = weightY + 32 + 20;

  g_tft.fillRect(0, weightY, kW, 32, ST7735_BLACK);
  g_tft.fillRect(0, titleY, kW, h, ST7735_BLACK);

  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  g_tft.setCursor(weightX, weightY);
  g_tft.setTextSize(intSize);
  g_tft.print(intStr);
  weightX += intWidth;

  g_tft.setCursor(weightX, weightY + 16);
  g_tft.setTextSize(dotSize);
  g_tft.print(".");
  weightX += dotWidth;

  g_tft.setCursor(weightX, weightY + 16);
  g_tft.setTextSize(decSize);
  g_tft.print(decStr);

  g_tft.setCursor(titleX, titleY);
  g_tft.setTextSize(titleSize);
  g_tft.print(title);
}

// ---------------------------------------------------------------------------
// SCREENSAVER layout: H/M/S/centiseconds, each row independently diffed
// (ported from Display::displayScreensaver).
// ---------------------------------------------------------------------------

struct {
  uint32_t h = 0xFFFFFFFF;
  uint32_t m = 0xFFFFFFFF;
  uint32_t s = 0xFFFFFFFF;
  uint32_t cs = 0xFFFFFFFF;
} g_saver;

void drawScreensaver(uint32_t h, uint32_t m, uint32_t s, uint32_t ms, bool force) {
  if (force) {
    g_saver = {};
  }

  constexpr int16_t startY = 12;
  constexpr int16_t rowHeight = 40;
  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);

  auto drawRow = [&](uint32_t value, uint32_t &last, int16_t rowIndex,
                     const char *fmt, uint8_t size, int16_t rowH) {
    if (value == last) return;
    char buf[10];
    snprintf(buf, sizeof(buf), fmt, static_cast<unsigned long>(value));
    g_tft.setTextSize(size);
    int16_t x, y;
    uint16_t w, hgt;
    g_tft.getTextBounds(buf, 0, 0, &x, &y, &w, &hgt);
    int16_t drawX = kW - w;
    int16_t drawY = startY + rowHeight * rowIndex;
    g_tft.fillRect(0, drawY, kW, rowH, ST7735_BLACK);
    g_tft.setCursor(drawX, drawY);
    g_tft.print(buf);
    last = value;
  };

  drawRow(h, g_saver.h, 0, "%02lu", 4, 32);
  drawRow(m, g_saver.m, 1, "%02lu", 4, 32);
  drawRow(s, g_saver.s, 2, "%02lu", 4, 32);

  uint32_t cs = ms / 10;
  if (cs != g_saver.cs) {
    char buf[10];
    snprintf(buf, sizeof(buf), ".%02lu", static_cast<unsigned long>(cs));
    g_tft.setTextSize(2);
    int16_t x, y;
    uint16_t w, hgt;
    g_tft.getTextBounds(buf, 0, 0, &x, &y, &w, &hgt);
    int16_t drawY = startY + rowHeight * 3;
    g_tft.fillRect(0, drawY, kW, 16, ST7735_BLACK);
    g_tft.setCursor(kW - w, drawY);
    g_tft.print(buf);
    g_saver.cs = cs;
  }
}

// ---------------------------------------------------------------------------
// DEBUG layout: IP address top row, raw ADC + stability flag bottom row.
// ---------------------------------------------------------------------------

struct { char ip[16] = ""; int32_t rawAdc = 0; bool stable = false; bool haveAny = false; } g_debug;

void drawTwoRow(const char *top, const char *bottom, bool force) {
  int16_t x, y;
  uint16_t w, h;

  if (force) {
    g_tft.setTextSize(2);
    g_tft.getTextBounds("X", 0, 0, &x, &y, &w, &h);
    g_tft.fillRect(0, 0, kW, h, ST7735_BLACK);
    g_tft.fillRect(0, kH - h, kW, h, ST7735_BLACK);
  }

  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  g_tft.setTextSize(2);
  g_tft.getTextBounds(top, 0, 0, &x, &y, &w, &h);
  g_tft.fillRect(0, 0, kW, h, ST7735_BLACK);
  g_tft.setCursor((kW - w) / 2, 0);
  g_tft.print(top);

  g_tft.getTextBounds(bottom, 0, 0, &x, &y, &w, &h);
  g_tft.fillRect(0, kH - h, kW, h, ST7735_BLACK);
  g_tft.setCursor((kW - w) / 2, kH - h);
  g_tft.print(bottom);
}

void drawDebugLayout(const char *ip, int32_t rawAdc, bool stable, bool force) {
  char bottom[24];
  snprintf(bottom, sizeof(bottom), "%ld %s", static_cast<long>(rawAdc), stable ? "S" : "P");

  bool ipChanged = strncmp(ip, g_debug.ip, sizeof(g_debug.ip)) != 0;
  bool bottomChanged = rawAdc != g_debug.rawAdc || stable != g_debug.stable;

  if (!force && !ipChanged && !bottomChanged && g_debug.haveAny) {
    return;
  }

  const char *ipDisplay = (ip[0] != '\0') ? ip : "---";
  drawTwoRow(ipDisplay, bottom, force);

  strncpy(g_debug.ip, ip, sizeof(g_debug.ip) - 1);
  g_debug.ip[sizeof(g_debug.ip) - 1] = '\0';
  g_debug.rawAdc = rawAdc;
  g_debug.stable = stable;
  g_debug.haveAny = true;
}

// ---------------------------------------------------------------------------
// OTA_UPDATE layout: "UPDATE" top row, percent bottom row.
// ---------------------------------------------------------------------------

struct { uint8_t percent = 0xFF; } g_ota;

void drawOtaLayout(uint8_t percent, bool force) {
  if (!force && percent == g_ota.percent) {
    return;
  }
  char bottom[8];
  snprintf(bottom, sizeof(bottom), "%3u%%", percent);
  drawTwoRow("UPDATE", bottom, force);
  g_ota.percent = percent;
}

// ---------------------------------------------------------------------------
// Mode dispatch
// ---------------------------------------------------------------------------

void renderMode(const DisplayCommand &cmd, bool force) {
  switch (cmd.mode) {
    case DisplayMode::BOOT:
      drawCentered("Eureka", 2, ST7735_WHITE, force);
      break;

    case DisplayMode::IDLE:
      drawIdleLayout(cmd.current_grams, cmd.connection_indicator_color, force);
      break;

    case DisplayMode::CONFIRM:
      drawConfirmLayout(cmd.target_grams, force);
      break;

    case DisplayMode::TARE:
      drawCentered("T", 2, ST7735_WHITE, force);
      break;

    case DisplayMode::GRINDING:
    case DisplayMode::TOPUP:
    case DisplayMode::STOPPING:
    case DisplayMode::FINALIZE: {
      uint16_t defCurrent, defTarget, defTime;
      defaultColorsFor(cmd.mode, &defCurrent, &defTarget, &defTime);
      uint16_t current = cmd.current_color != 0 ? cmd.current_color : defCurrent;
      uint16_t target = cmd.target_color != 0 ? cmd.target_color : defTarget;
      uint16_t time = cmd.time_color != 0 ? cmd.time_color : defTime;
      drawGrindingBlock(cmd.current_grams, cmd.target_grams, cmd.elapsed_s, current,
                        target, time, cmd.connection_indicator_color, force);
      break;
    }

    case DisplayMode::SCREENSAVER:
      drawScreensaver(cmd.idle_h, cmd.idle_m, cmd.idle_s, cmd.idle_ms, force);
      break;

    case DisplayMode::DEBUG:
      drawDebugLayout(cmd.debug_ip, cmd.debug_raw_adc, cmd.debug_stable, force);
      break;

    case DisplayMode::OTA_UPDATE:
      drawOtaLayout(cmd.ota_percent, force);
      break;
  }
}

void displayTaskFn(void *) {
  panelBegin();
  xEventGroupSetBits(g_sys_events, kDisplayReadyBit);

  // Dosing task's own BOOT state is transient and never reaches the mailbox
  // (it transitions to IDLE and calls sendDisplayCommand() for the first
  // time only after that -- see DosingTask.cpp), so this task owns showing
  // something before the first real command arrives, mirroring the old
  // setupDisplay()'s "Eureka" splash.
  drawCentered("Eureka", 2, ST7735_WHITE, true);

  DisplayCommand last{};
  bool haveLast = false;

  const TickType_t frameFloor = pdMS_TO_TICKS(33);  // §6.4
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    DisplayCommand cmd;
    // Block up to the frame floor for a new command; this both rate-limits
    // needless redraw work and lets the task idle when nothing changed.
    if (xQueueReceive(g_display_mailbox, &cmd, frameFloor) == pdTRUE) {
      bool modeChanged = !haveLast || cmd.mode != last.mode;
      if (modeChanged) {
        g_tft.fillScreen(ST7735_BLACK);
        g_lastConnColor = 0;  // a freshly-cleared screen has no indicator yet
      }
      renderMode(cmd, modeChanged);
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
