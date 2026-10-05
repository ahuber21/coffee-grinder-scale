/**
 * ST7735 rendering for the Display task, driving the 80x160 panel with
 * dirty-rect redraws: every layout diffs the new value against its own
 * "last drawn" cache and fills only the pixels that changed, because a
 * full clear every frame visibly flickers on this panel. `renderMode`
 * dispatches to one layout per screen; a mode change forces a full redraw.
 *
 * `DisplayCommand` colors arrive as 0 when a producer leaves them unset;
 * 0 means "use the mode's default" (`defaultColorsFor()`), never
 * black-on-black text.
 *
 * Without ARDUINO the same drawing code compiles against `FakeCanvas`, an
 * in-memory RGB565 stand-in for `Adafruit_ST7735`, with `millis()` and
 * `random()` shims; the FreeRTOS and panel plumbing is compiled out and a
 * `main()` at the bottom renders a scripted scenario to frame dumps. See
 * `tools/display_sim/README.md`.
 */

#include "DisplayTask.h"

#ifdef ARDUINO
#include <Adafruit_ST7735.h>
#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include "DisplaySimCanvas.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

#include "Messages.h"

#ifdef ARDUINO
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"
#endif

namespace {

constexpr int16_t kW = 80;
constexpr int16_t kH = 160;

#ifdef ARDUINO
SPIClass g_spi(HSPI);
Adafruit_ST7735 g_tft(&g_spi, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RESET_PIN);
#else
FakeCanvas g_tft;
#endif

// --- Palette (RGB565), matched to iOS system colors ------------------------------

constexpr uint16_t kColorSecondary = 0x9CD3;    ///< #98989D -- supporting text.
constexpr uint16_t kColorAccentBlue = 0x0C3F;   ///< #0A84FF -- active/primary action.
constexpr uint16_t kColorAccentGreen = 0x368B;  ///< #30D158 -- success/finalize.
constexpr uint16_t kColorOtaTrack = 0x0842;     ///< Near-black navy -- OTA screen's unfilled region.
constexpr uint16_t kColorOtaBlue = 0x0B7F;      ///< The OTA screen's own, more saturated blue.
constexpr uint16_t kColorCoffeePile = 0x4144;   ///< #3E2723, dark roast -- GRINDING's rising pile.
constexpr uint16_t kColorCoffeeLight = 0x6A67;  ///< #6F4E37 -- lighter roast, half the falling clumps.

/** Linearly interpolates two RGB565 colors by `t` (clamped to 0..1). */
uint16_t lerpColor565(uint16_t a, uint16_t b, float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + static_cast<int>((br - ar) * t);
  int g = ag + static_cast<int>((bg - ag) * t);
  int bl = ab + static_cast<int>((bb - ab) * t);
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

// --- Text helpers -------------------------------------------------------------

/** Copies `src` into a fixed buffer, always terminating it. */
template <size_t N>
void copyText(char (&dst)[N], const char *src) {
  strncpy(dst, src, N - 1);
  dst[N - 1] = '\0';
}

/** Pixel size of a rendered text string. */
struct TextExtent {
  int16_t w;
  int16_t h;
};

/** Measures `text` at `size`; leaves the canvas text size set to `size`. */
TextExtent measureText(const char *text, uint8_t size) {
  int16_t x, y;
  uint16_t w, h;
  g_tft.setTextSize(size);
  g_tft.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  return {static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

/** Rounds tiny +/-0.0x readings to 0.0 so load-cell noise around zero never flips "0.0" and "-0.0". */
float cleanZero(float grams) { return (grams > -0.05f && grams < 0.05f) ? 0.0f : grams; }

/** Splits |grams| into integer and decigram digit strings, plus a separate sign flag. */
void splitDecigrams(float grams, char *intStr, size_t intCap, char *decStr, size_t decCap,
                    bool *isNegative) {
  long totalDecigrams = lroundf(fabsf(grams) * 10.0f);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;
  *isNegative = grams < -0.05f;
  snprintf(intStr, intCap, "%d", intPart);
  snprintf(decStr, decCap, "%d", decPart);
}

/**
 * Prints a weight as big integer digits, a dot and a small decigram digit,
 * horizontally centered with its top at `y`; a negative value gets a small
 * minus sign. The caller sets the text color.
 */
void printCenteredReadout(float grams, int16_t y, uint8_t dotSize) {
  constexpr uint8_t intSize = 4;
  constexpr uint8_t decSize = 2;
  constexpr uint8_t minusSize = 2;

  char intStr[10];
  char decStr[5];
  bool isNegative;
  splitDecigrams(grams, intStr, sizeof(intStr), decStr, sizeof(decStr), &isNegative);

  int16_t intWidth = strlen(intStr) * 6 * intSize;
  int16_t decWidth = strlen(decStr) * 6 * decSize;
  int16_t minusWidth = isNegative ? (6 * minusSize) : 0;
  int16_t dotWidth = 6 * dotSize;
  int16_t x = (kW - (minusWidth + intWidth + dotWidth + decWidth)) / 2;

  if (isNegative) {
    g_tft.setCursor(x, y + 8);
    g_tft.setTextSize(minusSize);
    g_tft.print("-");
    x += minusWidth;
  }
  g_tft.setCursor(x, y);
  g_tft.setTextSize(intSize);
  g_tft.print(intStr);
  x += intWidth;

  g_tft.setCursor(x, y + 16);
  g_tft.setTextSize(dotSize);
  g_tft.print(".");
  x += dotWidth;

  g_tft.setCursor(x, y + 16);
  g_tft.setTextSize(decSize);
  g_tft.print(decStr);
}

/** Prints `text` in white over a one-pixel black shadow, so it reads over any background. */
void printShadowed(const char *text, int16_t x, int16_t y) {
  g_tft.setTextColor(ST7735_BLACK);
  g_tft.setCursor(x + 1, y + 1);
  g_tft.print(text);
  g_tft.setTextColor(ST7735_WHITE);
  g_tft.setCursor(x, y);
  g_tft.print(text);
}

// --- Falling-clumps background: GRINDING family + OTA_UPDATE ------------------------
// Squares fall into a pile that rises with `frac`; density and speed are live-tunable.

namespace clumps {
constexpr uint8_t kDefaultCount = 14;  ///< Clump count at a density multiplier of 1.0.
constexpr uint8_t kMaxCount = 42;      ///< Fixed array capacity; headroom for density to scale into.
constexpr int16_t kSize = 2;
constexpr uint32_t kFrameMs = 70;  ///< ~14fps for animation-only ticks.

struct Clump {
  float x = 0.0f;
  float y = 0.0f;
  float speed = 1.0f;  ///< px per kFrameMs tick, before g_clumpGravity scales it.
  bool lightShade = false;
};
}  // namespace clumps

// Multipliers refreshed from SettingsSnapshot once per frame (ARDUINO only);
// they default to 1.0 so the host simulator, which has no settings mailbox, still builds.
float g_clumpDensity = 1.0f;  ///< Multiplies kDefaultCount, clamped into [0, kMaxCount].
float g_clumpGravity = 1.0f;  ///< Multiplies every clump's fall speed.

/** The shared clump field's animation state; one screen uses it at a time. */
struct {
  clumps::Clump items[clumps::kMaxCount];
  uint8_t activeCount = 0;  ///< How many of `items`, from index 0, are currently falling.
  bool seeded = false;
  int16_t pileTopY = kH;  ///< Current pile surface; y=0 is the top of the screen.
  uint32_t lastDrawMs = 0;
} g_clumpField;

/** (Re)spawns one clump at a random x, staggered above the screen so falls don't sync up. */
void seedClump(clumps::Clump &c) {
  c.x = static_cast<float>(random(0, kW - clumps::kSize));
  c.y = static_cast<float>(random(-kH, 0));
  c.speed = random(70, 160) / 100.0f;
  c.lightShade = random(0, 2) == 0;
}

/**
 * Erases one clump's last-drawn footprint, clipped to the track (a pile
 * repaint already owns anything inside it). Also needed when density
 * shrinks, so a dropped clump leaves no stray pixel.
 */
void eraseClump(const clumps::Clump &c, int16_t pileTopY, uint16_t trackColor) {
  int16_t oldY = static_cast<int16_t>(c.y);
  int16_t eraseTop = oldY > 0 ? oldY : 0;
  int16_t eraseBottom = oldY + clumps::kSize < pileTopY ? oldY + clumps::kSize : pileTopY;
  if (eraseBottom > eraseTop) {
    g_tft.fillRect(static_cast<int16_t>(c.x), eraseTop, clumps::kSize, eraseBottom - eraseTop,
                   trackColor);
  }
}

/** Resizes the active clump set to `wantCount`; returns true if it erased dropped clumps. */
bool resizeClumpSet(uint8_t wantCount, bool force, uint16_t trackColor) {
  if (force || !g_clumpField.seeded) {
    // Seed every slot, so one that becomes active later already sits at a plausible spot.
    for (auto &c : g_clumpField.items) seedClump(c);
    g_clumpField.seeded = true;
    g_clumpField.activeCount = wantCount;
    g_clumpField.pileTopY = kH;  // Forces the full pile band to repaint.
    return false;
  }
  if (wantCount > g_clumpField.activeCount) {
    for (uint8_t i = g_clumpField.activeCount; i < wantCount; ++i) {
      seedClump(g_clumpField.items[i]);
    }
    g_clumpField.activeCount = wantCount;
    return false;
  }
  if (wantCount < g_clumpField.activeCount) {
    for (uint8_t i = wantCount; i < g_clumpField.activeCount; ++i) {
      eraseClump(g_clumpField.items[i], g_clumpField.pileTopY, trackColor);
    }
    g_clumpField.activeCount = wantCount;
    return true;
  }
  return false;
}

/** Repaints the pile band between its previous and new surface. */
void repaintPile(int16_t pileTopY, bool force, uint16_t pileColor, uint16_t trackColor,
                 bool pileColorMayShift) {
  int16_t prevTopY = force ? kH : g_clumpField.pileTopY;
  if (pileTopY < prevTopY) {
    if (pileColorMayShift && pileTopY < kH) {
      g_tft.fillRect(0, pileTopY, kW, kH - pileTopY, pileColor);
    } else {
      g_tft.fillRect(0, pileTopY, kW, prevTopY - pileTopY, pileColor);
    }
  } else if (pileTopY > prevTopY) {
    g_tft.fillRect(0, prevTopY, kW, pileTopY - prevTopY, trackColor);
  }
  g_clumpField.pileTopY = pileTopY;
}

/** Advances every active clump one animation tick, erasing and redrawing it. */
void advanceClumps(int16_t pileTopY, uint16_t pileColor, uint16_t trackColor,
                   uint16_t clumpLightColor) {
  for (uint8_t i = 0; i < g_clumpField.activeCount; ++i) {
    clumps::Clump &c = g_clumpField.items[i];
    eraseClump(c, pileTopY, trackColor);

    c.y += c.speed * g_clumpGravity * (static_cast<float>(clumps::kFrameMs) / 16.0f);
    if (static_cast<int16_t>(c.y) + clumps::kSize >= pileTopY) {
      seedClump(c);
      continue;
    }
    int16_t newTop = static_cast<int16_t>(c.y) > 0 ? static_cast<int16_t>(c.y) : 0;
    int16_t newBottom = static_cast<int16_t>(c.y) + clumps::kSize;
    if (newBottom > newTop) {
      g_tft.fillRect(static_cast<int16_t>(c.x), newTop, clumps::kSize, newBottom - newTop,
                     c.lightShade ? clumpLightColor : pileColor);
    }
  }
}

/**
 * Draws or advances the falling-clumps background in this mode's palette:
 * `clumpLightColor` tints half the clumps, the rest use `pileColor`. Returns
 * true if it drew anything (pile moved or an animation tick was due), in
 * which case callers must redraw whatever they draw on top.
 * `pileColorMayShift` is for a pile whose color changes over time (OTA): each
 * grown band then re-floods the whole pile, avoiding visible banding.
 */
bool drawClumpField(float frac, uint16_t pileColor, uint16_t trackColor, uint16_t clumpLightColor,
                    bool force, bool pileColorMayShift = false) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  int16_t pileTopY = kH - static_cast<int16_t>(frac * kH + 0.5f);

  long wantedCount = lroundf(clumps::kDefaultCount * g_clumpDensity);
  if (wantedCount < 0) wantedCount = 0;
  if (wantedCount > clumps::kMaxCount) wantedCount = clumps::kMaxCount;
  bool densityShrunk = resizeClumpSet(static_cast<uint8_t>(wantedCount), force, trackColor);

  uint32_t now = millis();
  bool animTick = force || (now - g_clumpField.lastDrawMs) >= clumps::kFrameMs;
  bool pileMoved = force || pileTopY != g_clumpField.pileTopY;
  if (!animTick && !pileMoved && !densityShrunk) {
    return false;
  }
  // Only a real animation tick advances the clock; a pile move alone is far more
  // frequent and would keep pushing the next tick back, starving the animation.
  if (animTick) {
    g_clumpField.lastDrawMs = now;
  }
  if (pileMoved) {
    repaintPile(pileTopY, force, pileColor, trackColor, pileColorMayShift);
  }
  if (animTick) {
    advanceClumps(pileTopY, pileColor, trackColor, clumpLightColor);
  }
  return true;
}

/**
 * Repaints rows [y, y+h) split at the pile boundary (track above, pile
 * below), so a text field drawn on top stays consistent with the
 * background instead of punching an opaque box through it.
 */
void fillGrindBackground(int16_t y, int16_t h, uint16_t pileColor, uint16_t trackColor) {
  int16_t pileTopY = g_clumpField.pileTopY;
  int16_t trackEnd = pileTopY < y ? y : (pileTopY > y + h ? y + h : pileTopY);
  int16_t trackH = trackEnd - y;
  if (trackH > 0) g_tft.fillRect(0, y, kW, trackH, trackColor);
  if (h - trackH > 0) g_tft.fillRect(0, y + trackH, kW, h - trackH, pileColor);
}

/** True if rows [y, y+h) lie wholly on one side of the pile boundary, i.e. one flat color. */
bool bandOutsidePile(int16_t y, int16_t h) {
  int16_t pileTopY = g_clumpField.pileTopY;
  return pileTopY <= y || pileTopY >= y + h;
}

/** The flat color covering [y, y+h); only meaningful when `bandOutsidePile(y, h)`. */
uint16_t bandFlatColor(int16_t y, uint16_t pileColor, uint16_t trackColor) {
  return g_clumpField.pileTopY <= y ? pileColor : trackColor;
}

// --- Panel bring-up (real hardware only) --------------------------------------------
#ifdef ARDUINO

/// ledcSetup(1, 100, 8) gives an 8-bit duty register.
constexpr uint8_t kBacklightMaxDuty = 255;

void backlightPercent(uint8_t percent) {
  ledcWrite(1, (static_cast<uint32_t>(percent) * kBacklightMaxDuty) / 100u);
}

/** Initializes the SPI bus, the ST7735 panel, and the backlight PWM channel. */
void panelBegin() {
  g_spi.begin(DISPLAY_SCK_PIN, DISPLAY_MISO_PIN, DISPLAY_MOSI_PIN, DISPLAY_SS_PIN);
  g_tft.initR(INITR_MINI160x80);
  g_tft.setRotation(0);
  g_tft.invertDisplay(false);
  /*
   * This panel's color filter is BGR, but the driver assumes RGB for the
   * MINI160x80 tab, which swaps red and blue. Re-issue MADCTL with the BGR
   * bit plus the MX|MY mirror bits setRotation(0) sets, or the panel flips.
   */
  constexpr uint8_t kMadctlRotation0Bgr = ST77XX_MADCTL_MX | ST77XX_MADCTL_MY | ST7735_MADCTL_BGR;
  g_tft.sendCommand(ST77XX_MADCTL, &kMadctlRotation0Bgr, 1);
  ledcAttachPin(DISPLAY_BACKLIGHT_PIN, 1);
  ledcSetup(1, 100, 8);
  backlightPercent(100);
  g_tft.fillScreen(ST7735_BLACK);
}

/** Refreshes the clump multipliers from the latest settings snapshot, once per frame. */
void applyDisplaySettings() {
  SettingsSnapshot snap;
  if (xQueuePeek(g_settings_mailbox_display, &snap, 0) == pdTRUE) {
    g_clumpDensity = snap.display_clump_density;
    g_clumpGravity = snap.display_clump_gravity;
  }
}
#endif  // ARDUINO

// --- Connection indicator: small bottom-left dot; 0 = none ----------------------------

uint16_t g_lastConnColor = 0;

/** Erases the old dot, then draws the new one, so a stale dot never survives a change to 0. */
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

/** Cache for the generic centered text line shared by BOOT ("Eureka") and TARE ("T"). */
struct {
  char text[16] = "";
  uint8_t size = 0;
  uint16_t color = 0;
} g_centered;

/** Draws (or skips, if unchanged) BOOT/TARE's centered text line. */
void drawCentered(const char *text, uint8_t size, uint16_t color, bool force) {
  if (!force && strncmp(text, g_centered.text, sizeof(g_centered.text)) == 0 &&
      size == g_centered.size && color == g_centered.color) {
    return;
  }
  TextExtent extent = measureText(text, size);
  g_tft.fillRect(0, kH / 2 - extent.h / 2 - 2, kW, extent.h + 4, ST7735_BLACK);
  g_tft.setTextColor(color, ST7735_BLACK);
  g_tft.setCursor(kW / 2 - extent.w / 2, kH / 2 - extent.h / 2);
  g_tft.print(text);

  copyText(g_centered.text, text);
  g_centered.size = size;
  g_centered.color = color;
}

// --- IDLE layout: big centered weight readout, dot anchored at x=60 ------------------

/** Cache of IDLE's last-drawn weight text and layout, for dirty-rect diffing. */
struct {
  char text[16] = "";
  int16_t intStartX = -1;
  bool isNegative = false;
  bool wasWide = false;
} g_idle;

constexpr int16_t kIdleReadoutY = (kH - 32) / 2;

/** Readouts too wide for the one-decimal layout show their whole integer value. */
void drawIdleWide(float currentGrams, bool force) {
  char text[16];
  snprintf(text, sizeof(text), "%.0f", currentGrams);
  if (!force && g_idle.wasWide && strcmp(g_idle.text, text) == 0) {
    return;
  }
  g_tft.fillRect(0, kIdleReadoutY, kW, 32, ST7735_BLACK);
  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  TextExtent extent = measureText(text, 3);
  g_tft.setCursor((kW - extent.w) / 2, (kH - extent.h) / 2);
  g_tft.print(text);
  copyText(g_idle.text, text);
  g_idle.intStartX = -1;
  g_idle.wasWide = true;
}

/** The normal readout: the integer part grows leftward from a dot anchored at x=60. */
void drawIdleNormal(float currentGrams, bool force) {
  float displayGrams = cleanZero(currentGrams);
  char text[16];
  snprintf(text, sizeof(text), "%5.1f", displayGrams);
  if (!force && strcmp(text, g_idle.text) == 0) {
    return;
  }

  char intStr[10];
  char decStr[5];
  bool isNegative;
  splitDecigrams(displayGrams, intStr, sizeof(intStr), decStr, sizeof(decStr), &isNegative);

  constexpr uint8_t intSize = 4;
  constexpr uint8_t decSize = 2;
  constexpr uint8_t minusSize = 2;
  constexpr uint8_t dotSize = 1;

  int16_t intStartX = 60 - static_cast<int16_t>(strlen(intStr)) * 6 * intSize;

  if (g_idle.wasWide) {
    g_tft.fillRect(0, kIdleReadoutY, kW, 32, ST7735_BLACK);
  } else if (g_idle.intStartX != -1) {
    // Erase only what the narrower readout no longer covers.
    if (intStartX > g_idle.intStartX) {
      g_tft.fillRect(g_idle.intStartX, kIdleReadoutY, intStartX - g_idle.intStartX, 32,
                     ST7735_BLACK);
    }
    if (g_idle.isNegative && !isNegative) {
      g_tft.fillRect(0, kIdleReadoutY + 8, 6 * minusSize, 8 * minusSize, ST7735_BLACK);
    }
  }

  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  if (isNegative) {
    g_tft.setCursor(0, kIdleReadoutY + 8);
    g_tft.setTextSize(minusSize);
    g_tft.print("-");
  }
  g_tft.setCursor(intStartX, kIdleReadoutY);
  g_tft.setTextSize(intSize);
  g_tft.print(intStr);

  g_tft.setCursor(60, kIdleReadoutY + 21);
  g_tft.setTextSize(dotSize);
  g_tft.print(".");

  g_tft.setCursor(60 + 6 * dotSize, kIdleReadoutY + 14);
  g_tft.setTextSize(decSize);
  g_tft.print(decStr);

  copyText(g_idle.text, text);
  g_idle.intStartX = intStartX;
  g_idle.isNegative = isNegative;
  g_idle.wasWide = false;
}

/** Draws (or skips, if unchanged) the IDLE screen's weight readout. */
void drawIdleLayout(float currentGrams, uint16_t connColor, bool force) {
  if (force) {
    g_idle = {};
  }
  if (fabsf(currentGrams) > 99.95f) {
    drawIdleWide(currentGrams, force);
  } else {
    drawIdleNormal(currentGrams, force);
  }
  drawConnectionIndicator(connColor);
}

// --- GRINDING/TOPUP/STOPPING/FINALIZE layout --------------------------------------------
// Current weight (top), target below, elapsed time at the bottom; the modes differ only in color.

/** Cache of the grinding-family layout's last-drawn text and colors. */
struct {
  char currentStr[16] = "";
  char targetStr[16] = "";
  char timeStr[16] = "";
  uint16_t currentColor = 0xFFFF;
  uint16_t targetColor = 0xFFFF;
  uint16_t timeColor = 0xFFFF;
} g_grind;

/// GRINDING's running-max display filter; kept apart from g_grind because it must survive the other modes.
float g_grindDisplayMaxGrams = 0.0f;

/**
 * Draws one centered text row over the clump background. A row redraws
 * when its value changed or the background moved. With only its own value
 * changed and the pile boundary away from its rows, `opaqueOverwrite`
 * lets it print with an opaque background instead of blanking first.
 */
void drawGrindRow(const char *text, const TextExtent &extent, int16_t y, uint16_t color,
                  bool redraw, bool opaqueOverwrite) {
  if (!redraw) {
    return;
  }
  if (opaqueOverwrite && bandOutsidePile(y, extent.h)) {
    g_tft.setTextColor(color, bandFlatColor(y, kColorCoffeePile, ST7735_BLACK));
  } else {
    fillGrindBackground(y, extent.h, kColorCoffeePile, ST7735_BLACK);
    g_tft.setTextColor(color);
  }
  g_tft.setCursor((kW - extent.w) / 2, y);
  g_tft.print(text);
}

/** Draws the GRINDING-family layout. */
void drawGrindingBlock(DisplayMode mode, float currentGrams, float targetGrams, float seconds,
                       uint16_t currentColor, uint16_t targetColor, uint16_t timeColor,
                       uint16_t connColor, bool force) {
  if (force) {
    g_grind = {};
    if (mode == DisplayMode::GRINDING) g_grindDisplayMaxGrams = 0.0f;
  }

  float displayGrams = cleanZero(currentGrams);
  // GRINDING shows a plausibility-bounded running maximum, so clump-impact jitter
  // can't make the number wobble. The other modes show the reading the decisions acted on.
  if (mode == DisplayMode::GRINDING) {
    constexpr float kMaxPlausibleJumpG = 1.0f;
    if (displayGrams > g_grindDisplayMaxGrams &&
        displayGrams - g_grindDisplayMaxGrams <= kMaxPlausibleJumpG) {
      g_grindDisplayMaxGrams = displayGrams;
    }
    displayGrams = g_grindDisplayMaxGrams;
  }
  float frac = targetGrams > 0.0f ? displayGrams / targetGrams : 0.0f;

  bool bgTick = drawClumpField(frac, kColorCoffeePile, ST7735_BLACK, kColorCoffeeLight, force);

  char currentStr[16];
  char targetStr[16];
  char timeStr[16];
  snprintf(currentStr, sizeof(currentStr), "%4.1f", displayGrams);
  snprintf(targetStr, sizeof(targetStr), "/%4.1f", targetGrams);
  snprintf(timeStr, sizeof(timeStr), "%4.1fs", seconds);

  bool currentChanged =
      strcmp(currentStr, g_grind.currentStr) != 0 || currentColor != g_grind.currentColor;
  bool targetChanged =
      strcmp(targetStr, g_grind.targetStr) != 0 || targetColor != g_grind.targetColor;
  bool timeChanged = strcmp(timeStr, g_grind.timeStr) != 0 || timeColor != g_grind.timeColor;

  if (!force && !bgTick && !currentChanged && !targetChanged && !timeChanged) {
    drawConnectionIndicator(connColor);
    return;
  }

  constexpr int16_t currentY = 38;
  constexpr uint8_t kRowTextSize = 2;
  bool background = force || bgTick;

  if (background || currentChanged) {
    /*
     * The opaque-overwrite shortcut needs the field to only ever widen,
     * which the running-max filter guarantees in GRINDING alone; a
     * shrinking unfiltered value elsewhere would leave stale digit pixels.
     */
    bool opaque = mode == DisplayMode::GRINDING && !background && bandOutsidePile(currentY, 32);
    if (opaque) {
      g_tft.setTextColor(currentColor, bandFlatColor(currentY, kColorCoffeePile, ST7735_BLACK));
    } else {
      fillGrindBackground(currentY, 32, kColorCoffeePile, ST7735_BLACK);
      g_tft.setTextColor(currentColor);
    }
    printCenteredReadout(displayGrams, currentY, /*dotSize=*/2);
  }

  TextExtent target = measureText(targetStr, kRowTextSize);
  const int16_t targetY = currentY + 32 + 10;
  drawGrindRow(targetStr, target, targetY, targetColor, background || targetChanged, !background);

  TextExtent time = measureText(timeStr, kRowTextSize);
  const int16_t timeY = targetY + time.h + 20;
  drawGrindRow(timeStr, time, timeY, timeColor, background || timeChanged, !background);

  copyText(g_grind.currentStr, currentStr);
  copyText(g_grind.targetStr, targetStr);
  copyText(g_grind.timeStr, timeStr);
  g_grind.currentColor = currentColor;
  g_grind.targetColor = targetColor;
  g_grind.timeColor = timeColor;

  drawConnectionIndicator(connColor);
}

/** Per-mode color scheme, used when a producer color field is 0/unset. */
void defaultColorsFor(DisplayMode mode, uint16_t *current, uint16_t *target, uint16_t *time) {
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
      *current = kColorSecondary;
      break;
  }
}

// --- CONFIRM layout: large target weight + "OK?" prompt -------------------------------

struct {
  float targetGrams = -1.0f;
} g_confirm;

/** Draws (or skips, if unchanged) the CONFIRM screen's target weight and prompt. */
void drawConfirmLayout(float targetGrams, bool force) {
  if (!force && g_confirm.targetGrams == targetGrams) {
    return;
  }
  g_confirm.targetGrams = targetGrams;

  constexpr int16_t weightY = 38;
  constexpr uint8_t titleSize = 3;
  const char *title = "OK?";
  TextExtent titleExtent = measureText(title, titleSize);
  int16_t titleX = (kW - titleExtent.w) / 2;
  int16_t titleY = weightY + 32 + 20;

  // The one screen whose purpose is a pending confirmation, so number and prompt get the accent color.
  g_tft.fillRect(0, weightY, kW, 32, ST7735_BLACK);
  g_tft.fillRect(0, titleY, kW, titleExtent.h + 8, ST7735_BLACK);

  g_tft.setTextColor(kColorAccentBlue, ST7735_BLACK);
  printCenteredReadout(targetGrams, weightY, /*dotSize=*/2);

  g_tft.setCursor(titleX, titleY);
  g_tft.setTextSize(titleSize);
  g_tft.print(title);
  g_tft.fillRoundRect(titleX, titleY + titleExtent.h + 4, titleExtent.w, 2, 1, kColorAccentBlue);
}

// SCREENSAVER has no layout: displayTaskFn() cuts the backlight on entry and restores it on
// exit, to spare the panel during long idle stretches, and the screen is left black.

// --- DEBUG layout: IP address top row, raw ADC and stability flag bottom row -----------

struct {
  char ip[16] = "";
  int32_t rawAdc = 0;
  bool stable = false;
  bool haveAny = false;
} g_debug;

/** Draws a top row and a bottom row, each centered, clearing both first on `force`. */
void drawTwoRow(const char *top, const char *bottom, bool force) {
  if (force) {
    TextExtent row = measureText("X", 2);
    g_tft.fillRect(0, 0, kW, row.h, ST7735_BLACK);
    g_tft.fillRect(0, kH - row.h, kW, row.h, ST7735_BLACK);
  }

  g_tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  TextExtent topExtent = measureText(top, 2);
  g_tft.fillRect(0, 0, kW, topExtent.h, ST7735_BLACK);
  g_tft.setCursor((kW - topExtent.w) / 2, 0);
  g_tft.print(top);

  TextExtent bottomExtent = measureText(bottom, 2);
  g_tft.fillRect(0, kH - bottomExtent.h, kW, bottomExtent.h, ST7735_BLACK);
  g_tft.setCursor((kW - bottomExtent.w) / 2, kH - bottomExtent.h);
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

  drawTwoRow((ip[0] != '\0') ? ip : "---", bottom, force);

  copyText(g_debug.ip, ip);
  g_debug.rawAdc = rawAdc;
  g_debug.stable = stable;
  g_debug.haveAny = true;
}

// --- OTA_UPDATE layout: falling clumps, blue rising to green ------------------------------

/** Cache of the OTA screen's last-drawn percent. */
struct {
  uint8_t percent = 0xFF;
} g_ota;

/**
 * OTA progress on the clump background: a blue-to-green pile rises with
 * `percent` while clumps keep falling, so the screen visibly moves
 * between the infrequent progress callbacks. drawClumpField throttles the
 * animation; the text redraws only when something drew or `percent` changed.
 */
void drawOtaLayout(uint8_t percent, bool force) {
  bool percentChanged = force || percent != g_ota.percent;
  g_ota.percent = percent;

  float frac = percent / 100.0f;
  // frac^2.2 keeps the pile a rich blue for most of the update and green only near the end;
  // a linear blend looked muddy teal by the midpoint.
  uint16_t pileColor = lerpColor565(kColorOtaBlue, kColorAccentGreen, powf(frac, 2.2f));
  // A cool cyan-white rather than neutral white keeps the clumps in the blue family.
  uint16_t clumpLightColor = lerpColor565(pileColor, 0xAFFF, 0.5f);
  bool bgTick = drawClumpField(frac, pileColor, kColorOtaTrack, clumpLightColor, force, true);

  if (!percentChanged && !bgTick) {
    return;
  }

  // Clear the background under the text first: a transparent print only overwrites glyph
  // pixels, so a clump caught mid-cell would otherwise show through the strokes.
  char pct[6];
  snprintf(pct, sizeof(pct), "%u%%", percent);
  TextExtent pctExtent = measureText(pct, 3);
  int16_t textX = (kW - pctExtent.w) / 2;
  int16_t textY = (kH - pctExtent.h) / 2;
  fillGrindBackground(textY, pctExtent.h + 1, pileColor, kColorOtaTrack);
  printShadowed(pct, textX, textY);

  // The small label above the number; a fast update can raise the pile this high before it finishes.
  const char *label = "UPDATING";
  TextExtent labelExtent = measureText(label, 1);
  int16_t labelX = (kW - labelExtent.w) / 2;
  int16_t labelY = textY - labelExtent.h - 8;
  fillGrindBackground(labelY, labelExtent.h + 1, pileColor, kColorOtaTrack);
  printShadowed(label, labelX, labelY);
}

// --- BOOT layout: wordmark, breathing accent bar, expanding ripples ------------------------

/** Cache of the BOOT splash's animation clock. */
struct {
  uint32_t startMs = 0;
} g_boot;

/**
 * Boot splash: the "Eureka" wordmark (drawn once) over a breathing accent
 * bar and two staggered expanding rings, a visible sign of life while the
 * rest of the system boots.
 */
void drawBootLayout(bool force) {
  uint32_t now = millis();
  if (force) {
    g_boot.startMs = now;
    drawCentered("Eureka", 2, ST7735_WHITE, true);
  }
  uint32_t elapsed = now - g_boot.startMs;

  // The bar breathes between 22 and 34px; erasing its full possible width first leaves no stale sliver.
  constexpr int16_t barY = kH / 2 + 14;
  constexpr int16_t barMaxW = 34;
  constexpr int16_t barMinW = 22;
  constexpr float kBarPeriodMs = 1400.0f;
  float barPhase = 0.5f + 0.5f * sinf(elapsed * (2.0f * static_cast<float>(M_PI)) / kBarPeriodMs);
  int16_t barW = barMinW + static_cast<int16_t>(barPhase * (barMaxW - barMinW));
  g_tft.fillRect(kW / 2 - barMaxW / 2, barY, barMaxW, 3, ST7735_BLACK);
  g_tft.fillRoundRect(kW / 2 - barW / 2, barY, barW, 3, 1, kColorAccentBlue);

  // Two staggered rings fade from accent blue to black as they grow. Their region sits clear
  // of the wordmark and bar, so a full-width clear each frame touches neither.
  constexpr int16_t cx = kW / 2;
  constexpr int16_t clearY = barY + 11;
  constexpr int16_t kMaxRadius = 24;
  constexpr int16_t cy = clearY + kMaxRadius;
  constexpr float kRipplePeriodMs = 1600.0f;

  g_tft.fillRect(0, clearY, kW, kH - clearY, ST7735_BLACK);
  for (int i = 0; i < 2; ++i) {
    float offset = i * kRipplePeriodMs * 0.5f;
    float phase = fmodf(static_cast<float>(elapsed) + offset, kRipplePeriodMs) / kRipplePeriodMs;
    int16_t radius = static_cast<int16_t>(phase * kMaxRadius);
    if (radius <= 0) continue;
    g_tft.drawCircle(cx, cy, radius, lerpColor565(kColorAccentBlue, ST7735_BLACK, phase));
  }
}

/** Dispatches one DisplayCommand to its mode's layout function. */
void renderMode(const DisplayCommand &cmd, bool force) {
  switch (cmd.mode) {
    case DisplayMode::BOOT:
      drawBootLayout(force);
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
      drawGrindingBlock(cmd.mode, cmd.current_grams, cmd.target_grams, cmd.elapsed_s, current,
                        target, time, cmd.connection_indicator_color, force);
      break;
    }

    case DisplayMode::SCREENSAVER:
      break;  // Left black by the mode-change clear; the backlight is handled by the task.

    case DisplayMode::DEBUG:
      drawDebugLayout(cmd.debug_ip, cmd.debug_raw_adc, cmd.debug_stable, force);
      break;

    case DisplayMode::OTA_UPDATE:
      drawOtaLayout(cmd.ota_percent, force);
      break;
  }
}

#ifdef ARDUINO
/// Hold the boot splash at least this long, since the boot gate often clears before it is even visible.
constexpr uint32_t kMinBootSplashMs = 1800;

/** Display task entry point: renders whatever DisplayCommand arrives, at ~60fps. */
void displayTaskFn(void *) {
  panelBegin();
  xEventGroupSetBits(g_sys_events, kDisplayReadyBit);

  // Dosing's BOOT state never reaches the mailbox, so the splash is seeded as `last`
  // and rendered through the same path as every other mode.
  DisplayCommand last{};
  last.mode = DisplayMode::BOOT;
  renderMode(last, true);

  uint32_t bootSplashStartMs = millis();
  bool havePendingCmd = false;
  DisplayCommand pendingCmd{};

  // Applies one command as the new screen state. Shared by the live and
  // flush-after-boot paths below so their mode-change handling can't drift apart.
  auto applyCommand = [&](const DisplayCommand &cmd) {
    bool modeChanged = cmd.mode != last.mode;
    if (modeChanged) {
      g_tft.fillScreen(ST7735_BLACK);
      g_lastConnColor = 0;  // A freshly cleared screen has no indicator.
      if (cmd.mode == DisplayMode::SCREENSAVER) {
        backlightPercent(0);
      } else if (last.mode == DisplayMode::SCREENSAVER) {
        backlightPercent(100);
      }
    }
    renderMode(cmd, modeChanged);
    last = cmd;
    havePendingCmd = false;
  };

  const TickType_t frameFloor = pdMS_TO_TICKS(16);  // ~60fps frame-rate floor.
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    applyDisplaySettings();

    DisplayCommand cmd;
    bool holdingBoot =
        last.mode == DisplayMode::BOOT && (millis() - bootSplashStartMs) < kMinBootSplashMs;

    // Blocking up to the frame floor rate-limits redraws and lets the task idle.
    if (xQueueReceive(g_display_mailbox, &cmd, frameFloor) == pdTRUE) {
      if (holdingBoot && cmd.mode != DisplayMode::BOOT) {
        // Keep the freshest command; the mailbox is single-slot, so an early one would go stale.
        pendingCmd = cmd;
        havePendingCmd = true;
      } else {
        applyCommand(cmd);
      }
    } else if (havePendingCmd && !holdingBoot) {
      applyCommand(pendingCmd);
    } else if (last.mode == DisplayMode::BOOT || last.mode == DisplayMode::OTA_UPDATE) {
      // These two modes animate continuously; every other mode is static between state changes.
      renderMode(last, false);
    }

    vTaskDelayUntil(&lastFrame, frameFloor);
  }
}
#endif  // ARDUINO

}  // namespace

#ifdef ARDUINO
void createDisplayTask() {
  xTaskCreatePinnedToCore(displayTaskFn, "Display", TaskConfig::kDisplayStackBytes, nullptr,
                          TaskConfig::kDisplayPriority, nullptr, TaskConfig::kDisplayCore);
}
#else
#include "DisplaySimMain.h"
#endif
