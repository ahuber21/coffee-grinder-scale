/**
 * Real ST7735 rendering for the Display task, pushing the existing
 * 80x160 panel as close to smooth/flicker-free as this hardware allows
 * -- no resolution upgrade is available, so all the headroom has to
 * come from how carefully pixels are redrawn.
 *
 * Redraw strategy: the original firmware already did the right thing
 * at the *within-a-screen* level -- every layout function diffs the
 * new value against a handful of static "what did we draw last time"
 * variables and issues `fillRect` only over the pixels that actually
 * need to change (e.g. only the shrinking digits of the integer part,
 * not the whole number). That discipline is kept and ported here
 * essentially unchanged: it's exactly the targeted partial/dirty-rect
 * redraw this hardware needs to look flicker-free, and rewriting it
 * from scratch would just be re-deriving the same pixel math with more
 * risk. What's new is a single mode-dispatch loop (one `DisplayCommand`
 * mailbox, one "did the mode change -> full clear; otherwise ->
 * per-field targeted redraw" path, no per-mode special case at the top
 * level) and a connection indicator drawn with a symmetric erase, so a
 * client disconnecting can never leave a stale dot on screen.
 *
 * Per-mode color fields (`current_color`/`target_color`/`time_color`)
 * are part of `DisplayCommand` so the producer can drive them, but
 * Dosing task doesn't populate them yet -- they arrive as 0 (black),
 * which would render invisible text. This task treats 0 as "unset" and
 * falls back to a sensible per-mode color scheme instead (white while
 * grinding, cyan while topping up/settling, green at finalize) -- see
 * `defaultColorsFor()`. A non-zero value from the producer always wins.
 */

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

// --- Accent palette (RGB565), matched to iOS system colors ------------------

constexpr uint16_t kColorPrimary = ST7735_WHITE;  ///< Main readouts: calm, neutral.
constexpr uint16_t kColorSecondary = 0x9CD3;      ///< #98989D -- supporting text.
constexpr uint16_t kColorTrack = 0x39C7;          ///< #3A3A3C -- progress bar track.
constexpr uint16_t kColorAccentBlue = 0x0C3F;     ///< #0A84FF -- active/primary action.
constexpr uint16_t kColorAccentGreen = 0x368B;    ///< #30D158 -- success/finalize.

/** Cache of the top-of-screen progress bar's last-drawn width/color. */
struct { int16_t lastWidth = -1; uint16_t lastColor = 0; } g_progress;

/**
 * Draws (or skips, if unchanged) a thin progress bar across the top of the
 * screen -- `frac` (0..1, clamped) of `color` over a dim track.
 */
void drawProgressBar(float frac, uint16_t color, bool force) {
  constexpr int16_t barH = 3;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  int16_t width = static_cast<int16_t>(frac * kW + 0.5f);

  if (!force && width == g_progress.lastWidth && color == g_progress.lastColor) {
    return;
  }
  if (force || color != g_progress.lastColor) {
    g_tft.fillRect(0, 0, kW, barH, kColorTrack);
    g_tft.fillRect(0, 0, width, barH, color);
  } else if (width > g_progress.lastWidth) {
    g_tft.fillRect(g_progress.lastWidth, 0, width - g_progress.lastWidth, barH, color);
  } else if (width < g_progress.lastWidth) {
    g_tft.fillRect(width, 0, g_progress.lastWidth - width, barH, kColorTrack);
  }
  g_progress.lastWidth = width;
  g_progress.lastColor = color;
}

// --- Panel bring-up ----------------------------------------------------------

// ledcSetup(1, 100, 8) is an 8-bit duty register (0-255); duty below is
// scaled from that same range, not a wider constant.
void backlightPercent(uint8_t percent) {
  uint32_t duty = (static_cast<uint32_t>(percent) * 255u) / 100u;
  ledcWrite(1, duty);
}

/** Initializes the SPI bus, the ST7735 panel, and the backlight PWM channel. */
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

// --- Connection indicator: small bottom-left dot; 0 = none -----------------

uint16_t g_lastConnColor = 0;

/** Symmetric erase-then-draw so a stale dot never survives a color change to 0. */
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

// --- Small formatting helpers ------------------------------------------------

// Rounds tiny +/-0.0x readings to a clean 0.0 so the display never
// flickers between "0.0" and "-0.0" from load-cell noise around zero.
float cleanZero(float grams) { return (grams > -0.05f && grams < 0.05f) ? 0.0f : grams; }

/** Splits |grams| into integer/decigram strings and a separate sign flag. */
void splitDecigrams(float grams, char *intStr, size_t intCap, char *decStr,
                     size_t decCap, bool *isNegative) {
  long totalDecigrams = lroundf(fabsf(grams) * 10.0f);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;
  *isNegative = grams < -0.05f;
  snprintf(intStr, intCap, "%d", intPart);
  snprintf(decStr, decCap, "%d", decPart);
}

/*
 * Generic centered single line of text -- used for BOOT ("Eureka") and
 * TARE ("T"). Both are entered via a mode change (which already
 * triggers a full screen clear at the call site), so a single shared
 * "last drawn" cache is safe -- it's always invalidated by `force`
 * before either mode's first frame is drawn.
 */
struct { char text[16] = ""; uint8_t size = 0; uint16_t color = 0; } g_centered;

/** Draws (or skips, if unchanged) BOOT/TARE's centered text line. */
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

// --- IDLE layout: big centered weight readout, dot anchored at x=60 --------

/** Cache of IDLE's last-drawn weight text/layout, for dirty-rect diffing. */
struct {
  char text[16] = "";
  int16_t intStartX = -1;
  bool isNegative = false;
  bool wasWide = false;
} g_idle;

/** Draws (or skips, if unchanged) the IDLE screen's weight readout. */
void drawIdleLayout(float currentGrams, uint16_t connColor, bool force) {
  if (force) {
    g_idle = {};
  }

  constexpr int16_t currentY = (kH - 32) / 2;

  if (fabsf(currentGrams) > 99.95f) {
    // Too wide for the normal one-decimal layout -- show the full
    // integer value instead.
    char text[16];
    snprintf(text, sizeof(text), "%.0f", currentGrams);
    if (!force && g_idle.wasWide && strcmp(g_idle.text, text) == 0) {
      drawConnectionIndicator(connColor);
      return;
    }
    g_tft.fillRect(0, currentY, kW, 32, ST7735_BLACK);
    g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
    g_tft.setTextSize(3);
    int16_t x, y;
    uint16_t w, h;
    g_tft.getTextBounds(text, 0, 0, &x, &y, &w, &h);
    g_tft.setCursor((kW - w) / 2, (kH - h) / 2);
    g_tft.print(text);
    strncpy(g_idle.text, text, sizeof(g_idle.text) - 1);
    g_idle.text[sizeof(g_idle.text) - 1] = '\0';
    g_idle.intStartX = -1;
    g_idle.wasWide = true;
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

    if (g_idle.wasWide) {
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
    g_idle.wasWide = false;
  }

  drawConnectionIndicator(connColor);
}

// --- GRINDING/TOPUP/STOPPING/FINALIZE layout --------------------------------
// Current weight (top), target below, elapsed time at the bottom. Shared by
// every mode in this family -- they only differ in color.

/** Cache of the grinding-family layout's last-drawn text/colors. */
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

/** Draws (or skips, per-field, if unchanged) the GRINDING-family layout. */
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

  // Target, below current.
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

  // Time, at the bottom.
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

  float frac = targetGrams > 0.0f ? displayGrams / targetGrams : 0.0f;
  drawProgressBar(frac, currentColor, force);
  drawConnectionIndicator(connColor);
}

/** Per-mode color scheme -- used when a producer color field is 0/unset. */
void defaultColorsFor(DisplayMode mode, uint16_t *current, uint16_t *target,
                      uint16_t *time) {
  *target = kColorSecondary;
  *time = kColorSecondary;
  switch (mode) {
    case DisplayMode::TOPUP:
    case DisplayMode::STOPPING:
      *current = kColorAccentBlue;
      break;
    case DisplayMode::FINALIZE:
      *current = kColorAccentGreen;
      break;
    default:
      *current = kColorPrimary;
      break;
  }
}

// --- CONFIRM layout: large target weight + "OK?" prompt --------------------
// Targeted fillRect over just the two text regions, not a full-screen
// clear, to avoid visible flicker on every redraw.

struct { float targetGrams = -1.0f; } g_confirm;

/** Draws (or skips, if unchanged) the CONFIRM screen's target weight + prompt. */
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

  // Accent-colored: this is the one screen whose whole purpose is a
  // pending confirmation, so both the number and the prompt get the
  // primary-action color rather than the neutral readout white.
  g_tft.fillRect(0, weightY, kW, 32, ST7735_BLACK);
  g_tft.fillRect(0, titleY, kW, h + 8, ST7735_BLACK);

  g_tft.setTextColor(kColorAccentBlue, ST7735_BLACK);
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
  g_tft.fillRoundRect(titleX, titleY + h + 4, w, 2, 1, kColorAccentBlue);
}

// --- SCREENSAVER: backlight off, screen left black ---------------------
//
// The point is reducing panel wear/burn-in during long idle stretches,
// not giving the idle time somewhere else to display -- an always-on
// clock would just move the static-content problem rather than solve
// it. displayTaskFn() cuts the backlight on entry and restores it on
// exit; there is nothing to draw here.

// --- DEBUG layout: IP address top row, raw ADC + stability flag bottom row -

struct { char ip[16] = ""; int32_t rawAdc = 0; bool stable = false; bool haveAny = false; } g_debug;

/** Draws a top row and a bottom row, each right-aligned, clearing both on `force`. */
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

/** Draws (or skips, if unchanged) the DEBUG screen's IP/ADC/stability rows. */
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

// --- OTA_UPDATE layout: "UPDATE" top row, percent bottom row ---------------

struct { uint8_t percent = 0xFF; } g_ota;

/** Draws (or skips, if unchanged) the OTA_UPDATE screen's progress percent. */
void drawOtaLayout(uint8_t percent, bool force) {
  if (!force && percent == g_ota.percent) {
    return;
  }
  char bottom[8];
  snprintf(bottom, sizeof(bottom), "%3u%%", percent);
  drawTwoRow("UPDATE", bottom, force);
  // drawTwoRow's top row always clears y=0.. regardless of `force` (it
  // only skips its own one-time full-row clear, not the per-call text
  // redraw) -- always redraw the bar after it rather than relying on
  // drawProgressBar's own diff, which could otherwise stay skipped while
  // the pixels underneath were just wiped.
  drawProgressBar(percent / 100.0f, kColorAccentBlue, /*force=*/true);
  g_ota.percent = percent;
}

/** Dispatches one DisplayCommand to its mode's layout function. */
void renderMode(const DisplayCommand &cmd, bool force) {
  switch (cmd.mode) {
    case DisplayMode::BOOT:
      drawCentered("Eureka", 2, ST7735_WHITE, force);
      if (force) {
        // A small brand accent under the wordmark -- drawn once, since
        // this mode is otherwise static for its whole (brief) lifetime.
        g_tft.fillRoundRect(kW / 2 - 14, kH / 2 + 14, 28, 3, 1, kColorAccentBlue);
      }
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
      // Backlight is cut/restored by displayTaskFn() on mode entry/exit;
      // the screen itself is just left at the black the mode-change
      // clear already produced.
      break;

    case DisplayMode::DEBUG:
      drawDebugLayout(cmd.debug_ip, cmd.debug_raw_adc, cmd.debug_stable, force);
      break;

    case DisplayMode::OTA_UPDATE:
      drawOtaLayout(cmd.ota_percent, force);
      break;
  }
}

/** Display task entry point: renders whatever DisplayCommand arrives, at ~60fps. */
void displayTaskFn(void *) {
  panelBegin();
  xEventGroupSetBits(g_sys_events, kDisplayReadyBit);

  // Dosing task's BOOT state never reaches the mailbox (it transitions to
  // IDLE before sending its first command), so the boot splash is seeded
  // as the initial `last` state and rendered once through the same path
  // every other mode uses -- not a separate direct draw the mode-change
  // tracking below doesn't know about.
  DisplayCommand last{};
  last.mode = DisplayMode::BOOT;
  renderMode(last, true);

  const TickType_t frameFloor = pdMS_TO_TICKS(16);  // ~60fps frame-rate floor.
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    DisplayCommand cmd;
    // Block up to the frame floor for a new command; this both rate-limits
    // needless redraw work and lets the task idle when nothing changed.
    if (xQueueReceive(g_display_mailbox, &cmd, frameFloor) == pdTRUE) {
      bool modeChanged = cmd.mode != last.mode;
      if (modeChanged) {
        g_tft.fillScreen(ST7735_BLACK);
        g_lastConnColor = 0;  // a freshly-cleared screen has no indicator yet
        if (cmd.mode == DisplayMode::SCREENSAVER) {
          backlightPercent(0);
        } else if (last.mode == DisplayMode::SCREENSAVER) {
          backlightPercent(100);
        }
      }
      renderMode(cmd, modeChanged);
      last = cmd;
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
