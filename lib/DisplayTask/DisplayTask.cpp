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
 *
 * Native/host build (no ARDUINO): the same rendering code (every
 * constant/helper/`draw*` function/`renderMode`) compiles unchanged
 * against `FakeCanvas` -- an in-memory RGB565 framebuffer with the same
 * method surface `Adafruit_ST7735` provides for what this file actually
 * calls -- instead of `Adafruit_ST7735`/real SPI hardware, with `millis()`/
 * `random()` shims standing in for their Arduino counterparts. The FSM
 * plumbing (`displayTaskFn`/`createDisplayTask`/`panelBegin`, all
 * FreeRTOS/hardware-only) is compiled out; a small `main()` at the
 * bottom drives `renderMode()` through a scripted scenario and dumps
 * captured frames instead, so rendering changes can be reviewed as an
 * actual image/animation without needing the physical device or a
 * camera on it. See `tools/display_sim/README.md` for how to run it.
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

// --- Accent palette (RGB565), matched to iOS system colors ------------------

constexpr uint16_t kColorSecondary = 0x9CD3;      ///< #98989D -- supporting text.
constexpr uint16_t kColorAccentBlue = 0x0C3F;     ///< #0A84FF -- active/primary action.
constexpr uint16_t kColorAccentGreen = 0x368B;    ///< #30D158 -- success/finalize.
constexpr uint16_t kColorOtaTrack = 0x0842;       ///< Near-black navy -- OTA screen's unfilled region.
constexpr uint16_t kColorOtaBlue = 0x0B7F;        ///< Richer, more saturated blue than kColorAccentBlue -- this screen's own accent, not the shared UI one.
// Dark roast, not a light tan: kColorSecondary's gray reads fine on this
// (it was already tuned for a black background) but would wash out
// against anything lighter, and real ground coffee is dark anyway.
constexpr uint16_t kColorCoffeePile = 0x4144;     ///< #3E2723 -- GRINDING's rising pile.
constexpr uint16_t kColorCoffeeLight = 0x6A67;    ///< #6F4E37 -- lighter roast, half the falling clumps.

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

// --- Shared falling-clumps background: GRINDING family + OTA_UPDATE --------
// Small squares fall from the top and disappear into a solid pile rising
// from the bottom as `frac` climbs -- coffee grounds landing in the cup.
// Reused for OTA_UPDATE (both screens are fundamentally "one number
// climbing to its target/100%"), replacing that screen's old banded-liquid
// fill: each band's height was `filled / kBands` recomputed fresh every
// frame from the continuously-changing fill height, so integer truncation
// made the topmost (lightest) band's rendered height jitter by a pixel
// between adjacent frames -- the flicker the owner flagged. This has no
// bands: the pile is one flat fill, diffed against its own last-drawn
// boundary so only the newly (un)filled band is ever repainted, and the
// only per-frame motion is a handful of independent falling squares with
// their own float positions -- nothing here is recomputed from a size
// that changes underneath it.
namespace clumps {
constexpr uint8_t kCount = 14;
constexpr int16_t kSize = 2;
constexpr uint32_t kFrameMs = 70;  // ~14fps for animation-only ticks.

struct Clump {
  float x = 0.0f;
  float y = 0.0f;
  float speed = 1.0f;  ///< px per kFrameMs tick.
  bool lightShade = false;
};
}  // namespace clumps

/** Cache of the shared clump field's animation state -- one screen uses it at a time. */
struct {
  clumps::Clump items[clumps::kCount];
  bool seeded = false;
  int16_t pileTopY = kH;  ///< Current pile surface, y=0 at the top of the screen.
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
 * Draws (or advances) the falling-clumps background. `pileColor`/
 * `trackColor` are this mode's own palette (a flat coffee brown for
 * GRINDING, the existing frac-driven blue->green lerp for OTA);
 * `clumpLightColor` tints half the falling clumps for a bit of texture,
 * the other half drawn in `pileColor` itself. Returns true if it drew
 * anything this call (the pile boundary moved, or an animation tick was
 * due) -- callers must redraw anything they draw on top when this is
 * true, since either could have just painted over it.
 *
 * `pileColorMayShift`: GRINDING's pile color is a fixed constant, so only
 * ever painting the newly-grown band is correct and cheap. OTA's pile
 * color instead continuously shifts with `frac` (blue -> green) -- with
 * that same incremental-band approach, each band keeps whatever color it
 * was painted with, leaving visible "growth ring" stripes as the color
 * moves on since. Set this true to re-flood the whole pile with the
 * current `pileColor` on every move instead, trading a full-width refill
 * (still just a flat `fillRect`, no banding math) for a uniform color.
 */
bool drawClumpField(float frac, uint16_t pileColor, uint16_t trackColor,
                    uint16_t clumpLightColor, bool force, bool pileColorMayShift = false) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  int16_t pileTopY = kH - static_cast<int16_t>(frac * kH + 0.5f);

  if (force || !g_clumpField.seeded) {
    for (auto &c : g_clumpField.items) seedClump(c);
    g_clumpField.seeded = true;
    g_clumpField.pileTopY = kH;  // forces the full pile band below to repaint
  }

  uint32_t now = millis();
  bool animTick = force || (now - g_clumpField.lastDrawMs) >= clumps::kFrameMs;
  bool pileMoved = force || pileTopY != g_clumpField.pileTopY;
  if (!animTick && !pileMoved) {
    return false;
  }
  // Only advance the animation clock on a real animation tick -- pileMoved
  // alone (the weight display updates far more often than every kFrameMs)
  // must not reset it, or frequent weight-only redraws would keep pushing
  // the next actual clump tick back and starve the animation entirely.
  if (animTick) {
    g_clumpField.lastDrawMs = now;
  }

  if (pileMoved) {
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

  if (animTick) {
    for (auto &c : g_clumpField.items) {
      int16_t oldY = static_cast<int16_t>(c.y);
      // Erase the old footprint only where it's still in the track --
      // the pile repaint above already overwrote anything now inside it.
      int16_t eraseTop = oldY > 0 ? oldY : 0;
      int16_t eraseBottom = oldY + clumps::kSize < pileTopY ? oldY + clumps::kSize : pileTopY;
      if (eraseBottom > eraseTop) {
        g_tft.fillRect(static_cast<int16_t>(c.x), eraseTop, clumps::kSize,
                        eraseBottom - eraseTop, trackColor);
      }

      c.y += c.speed * (static_cast<float>(clumps::kFrameMs) / 16.0f);
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
  return true;
}

/**
 * Repaints rows [y, y+h) split at the clump field's current pile boundary
 * -- trackColor above it, pileColor below -- so a text field redrawn on
 * top stays consistent with the background instead of punching an opaque
 * box through it.
 */
void fillGrindBackground(int16_t y, int16_t h, uint16_t pileColor, uint16_t trackColor) {
  int16_t pileTopY = g_clumpField.pileTopY;
  int16_t trackEnd = pileTopY < y ? y : (pileTopY > y + h ? y + h : pileTopY);
  int16_t trackH = trackEnd - y;
  if (trackH > 0) g_tft.fillRect(0, y, kW, trackH, trackColor);
  if (h - trackH > 0) g_tft.fillRect(0, y + trackH, kW, h - trackH, pileColor);
}

/** True if rows [y, y+h) lie entirely on one side of the pile boundary -- a single flat
 * color, safe for an opaque-background text overwrite instead of `fillGrindBackground`'s
 * split fill. */
bool bandOutsidePile(int16_t y, int16_t h) {
  int16_t pileTopY = g_clumpField.pileTopY;
  return pileTopY <= y || pileTopY >= y + h;
}

/** The single flat color covering [y, y+h) -- only meaningful when `bandOutsidePile(y, h)`. */
uint16_t bandFlatColor(int16_t y, uint16_t pileColor, uint16_t trackColor) {
  return g_clumpField.pileTopY <= y ? pileColor : trackColor;
}

// --- Panel bring-up (real hardware only) ------------------------------------
#ifdef ARDUINO

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
  // This physical panel's color filter is BGR, not the RGB the driver
  // library assumes for the MINI160x80 tab type -- confirmed live: a
  // color meant to read as blue (low red, low-mid green, full blue)
  // rendered as orange (full red, low-mid green, low blue), exactly
  // what swapping red and blue on that value would produce. Re-issuing
  // MADCTL corrects every color in the file at the source, rather than
  // swapping red/blue in every constant. Must OR in the same MX|MY
  // mirror bits setRotation(0) itself sets for this exact tab type
  // (confirmed by reading the vendored library's setRotation() source)
  // -- sending BGR alone clears them, which is what flipped the panel
  // upside down on the first attempt at this fix.
  constexpr uint8_t kMadctlRotation0Bgr =
      ST77XX_MADCTL_MX | ST77XX_MADCTL_MY | ST7735_MADCTL_BGR;
  g_tft.sendCommand(ST77XX_MADCTL, &kMadctlRotation0Bgr, 1);
  ledcAttachPin(DISPLAY_BACKLIGHT_PIN, 1);
  ledcSetup(1, 100, 8);
  backlightPercent(100);
  g_tft.fillScreen(ST7735_BLACK);
}
#endif  // ARDUINO

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
} g_grind;

// GRINDING's own running-max display filter (see drawGrindingBlock) --
// separate from g_grind since it must survive across the TOPUP/STOPPING/
// FINALIZE modes that share this same function but don't use it.
float g_grindDisplayMaxGrams = 0.0f;

/**
 * Draws the GRINDING-family layout. Each field (current/target/time) is
 * redrawn independently, on either a real change to that field or a
 * background animation tick (the falling clumps behind it may have moved
 * -- see drawClumpField -- leaving stale pixels a skipped field wouldn't
 * know to repaint). A field that's redrawn purely because its own value
 * changed, with no animation tick and the pile boundary nowhere near its
 * rows (bandOutsidePile), overwrites in place with an opaque text
 * background instead of blanking the whole field first: every one of
 * these fields is formatted to only grow in on-screen width as its value
 * rises (running-max weight, elapsed time, a fixed-width target), and
 * centered/right-anchored so a wider redraw always fully covers the
 * previous one -- see drawIdleLayout's intStartX tracking for the same
 * reasoning applied to a left-growing field. Skipping that blank-first
 * step is what actually matters here: at the ~sample rate this redraws,
 * doing it on every tick was the real source of visible flicker, far
 * more often than the ~14fps clump animation alone would ever need.
 * Genuinely idle screens (holding on FINALIZE, say) still cost nothing
 * between ticks, same as before.
 */
void drawGrindingBlock(DisplayMode mode, float currentGrams, float targetGrams, float seconds,
                       uint16_t currentColor, uint16_t targetColor,
                       uint16_t timeColor, uint16_t connColor, bool force) {
  if (force) {
    g_grind = {};
    if (mode == DisplayMode::GRINDING) g_grindDisplayMaxGrams = 0.0f;
  }

  float displayGrams = cleanZero(currentGrams);
  /*
   * GRINDING only: the real reading is genuinely noisy (load-cell jitter,
   * a falling clump's momentary extra force) and showing it directly reads
   * as a jumpy, non-monotonic number for a quantity that only ever
   * physically increases while grinding. TOPUP/STOPPING/FINALIZE keep
   * showing the real reading unfiltered -- those are the numbers the
   * stop/topup decisions actually acted on, and hiding a real settle or
   * cup-lift there would be actively misleading, not just less pretty.
   * Same plausibility bound as MainGrindModel's own addSample() (a
   * clump's impact reads heavier than its true settled mass for an
   * instant), reused here for the same physical reason -- this is a
   * separate, display-only filter that affects no real stop/topup
   * decision, not a change to that one.
   */
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

  constexpr uint8_t intSize = 4;
  constexpr uint8_t decSize = 2;
  constexpr uint8_t minusSize = 2;
  constexpr uint8_t dotSize = 2;
  constexpr int16_t currentY = 38;

  if (force || bgTick || currentChanged) {
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

    // The opaque-overwrite fast path relies on this field only ever
    // growing wider, which the running-max filter above only guarantees
    // in GRINDING -- TOPUP/STOPPING/FINALIZE show the real, unfiltered
    // (non-monotonic) reading on purpose, so a shrinking value there can
    // leave stale digit pixels behind a narrower opaque redraw. Confirmed
    // via tools/display_sim: injecting the same jitter runSegment uses
    // for those modes reproduced exactly this artifact before this guard.
    if (mode == DisplayMode::GRINDING && !force && !bgTick && bandOutsidePile(currentY, 32)) {
      g_tft.setTextColor(currentColor, bandFlatColor(currentY, kColorCoffeePile, ST7735_BLACK));
    } else {
      fillGrindBackground(currentY, 32, kColorCoffeePile, ST7735_BLACK);
      g_tft.setTextColor(currentColor);
    }

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

  if (force || bgTick || targetChanged) {
    if (!force && !bgTick && bandOutsidePile(targetY, h)) {
      g_tft.setTextColor(targetColor, bandFlatColor(targetY, kColorCoffeePile, ST7735_BLACK));
    } else {
      fillGrindBackground(targetY, h, kColorCoffeePile, ST7735_BLACK);
      g_tft.setTextColor(targetColor);
    }
    g_tft.setCursor((kW - w) / 2, targetY);
    g_tft.print(targetStr);
  }

  // Time, at the bottom.
  constexpr uint8_t timeSize = 2;
  g_tft.setTextSize(timeSize);
  g_tft.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);
  const int16_t timeY = targetY + h + 20;

  if (force || bgTick || timeChanged) {
    if (!force && !bgTick && bandOutsidePile(timeY, h)) {
      g_tft.setTextColor(timeColor, bandFlatColor(timeY, kColorCoffeePile, ST7735_BLACK));
    } else {
      fillGrindBackground(timeY, h, kColorCoffeePile, ST7735_BLACK);
      g_tft.setTextColor(timeColor);
    }
    g_tft.setCursor((kW - w) / 2, timeY);
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
      *current = kColorSecondary;
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

// --- OTA_UPDATE layout: falling clumps, blue rising to green ---------------

/** Cache of the OTA screen's last-drawn percent. */
struct { uint8_t percent = 0xFF; } g_ota;

/**
 * OTA progress on the same falling-clumps background as GRINDING (see
 * drawClumpField) -- a blue-to-green pile rises from the bottom as
 * `percent` climbs, with clumps continuously falling into it so the
 * screen keeps visibly moving between the relatively infrequent progress
 * callbacks ArduinoOTA actually delivers. Replaces this screen's old
 * banded-liquid fill, whose band height was recomputed from the
 * continuously-changing fill height every frame -- integer truncation
 * made the topmost band's rendered height jitter by a pixel between
 * adjacent frames, the flicker the owner flagged. drawClumpField owns
 * the animation throttle; this only redraws the percent/label text when
 * it actually drew something, or `percent` itself changed.
 */
void drawOtaLayout(uint8_t percent, bool force) {
  bool percentChanged = force || percent != g_ota.percent;
  g_ota.percent = percent;

  float frac = percent / 100.0f;
  // Biased toward frac^2.2 rather than frac itself, so the pile reads as
  // a rich blue for most of the update and only turns green in the last
  // stretch -- a straight linear blend started looking teal by the
  // midpoint, which read as muddy rather than "getting close."
  uint16_t pileColor = lerpColor565(kColorOtaBlue, kColorAccentGreen, powf(frac, 2.2f));
  // Toward a cool cyan-white, not neutral white, so falling clumps stay in
  // the same blue family instead of looking washed out.
  uint16_t clumpLightColor = lerpColor565(pileColor, 0xAFFF, 0.5f);
  bool bgTick =
      drawClumpField(frac, pileColor, kColorOtaTrack, clumpLightColor, force, true);

  if (!percentChanged && !bgTick) {
    return;
  }

  // Big centered percent, white-on-black-shadow so it reads over any band.
  // fillGrindBackground first (same helper GRINDING's text uses) -- a
  // transparent-background print only overwrites glyph pixels, so without
  // this a falling clump caught mid-cell would show through the gaps
  // between strokes until a clump happens to erase over that exact spot.
  char pct[6];
  snprintf(pct, sizeof(pct), "%u%%", percent);
  constexpr uint8_t pctSize = 3;
  int16_t x, y;
  uint16_t w, h;
  g_tft.setTextSize(pctSize);
  g_tft.getTextBounds(pct, 0, 0, &x, &y, &w, &h);
  int16_t textX = (kW - static_cast<int16_t>(w)) / 2;
  int16_t textY = (kH - static_cast<int16_t>(h)) / 2;
  fillGrindBackground(textY, static_cast<int16_t>(h) + 1, pileColor, kColorOtaTrack);
  g_tft.setTextColor(ST7735_BLACK);
  g_tft.setCursor(textX + 1, textY + 1);
  g_tft.print(pct);
  g_tft.setTextColor(ST7735_WHITE);
  g_tft.setCursor(textX, textY);
  g_tft.print(pct);

  // Small label above the number -- same shadow trick, since a fast
  // update can have the fill reach this high before it's done.
  constexpr uint8_t labelSize = 1;
  const char *label = "UPDATING";
  g_tft.setTextSize(labelSize);
  g_tft.getTextBounds(label, 0, 0, &x, &y, &w, &h);
  int16_t labelX = (kW - static_cast<int16_t>(w)) / 2;
  int16_t labelY = textY - static_cast<int16_t>(h) - 8;
  fillGrindBackground(labelY, static_cast<int16_t>(h) + 1, pileColor, kColorOtaTrack);
  g_tft.setTextColor(ST7735_BLACK);
  g_tft.setCursor(labelX + 1, labelY + 1);
  g_tft.print(label);
  g_tft.setTextColor(ST7735_WHITE);
  g_tft.setCursor(labelX, labelY);
  g_tft.print(label);
}

// --- BOOT layout: wordmark, breathing accent bar, expanding ripples -------

/** Cache of the BOOT splash's animation clock. */
struct { uint32_t startMs = 0; } g_boot;

/**
 * Boot splash: the "Eureka" wordmark (drawn once, per drawCentered's own
 * diffing) with a breathing accent bar underneath and a pair of
 * staggered rings expanding below that -- a small, deliberate sign of
 * life while the rest of the system finishes booting, rather than a
 * static screen that gives no indication anything is happening.
 */
void drawBootLayout(bool force) {
  uint32_t now = millis();
  if (force) {
    g_boot.startMs = now;
    drawCentered("Eureka", 2, ST7735_WHITE, true);
  }
  uint32_t elapsed = now - g_boot.startMs;

  // Accent bar: width breathes smoothly between 22 and 34px. Always
  // erased over its full possible width first so a shrinking bar never
  // leaves a stale sliver from a wider previous frame.
  constexpr int16_t barY = kH / 2 + 14;
  constexpr int16_t barMaxW = 34;
  constexpr int16_t barMinW = 22;
  constexpr float kBarPeriodMs = 1400.0f;
  float barPhase =
      0.5f + 0.5f * sinf(elapsed * (2.0f * static_cast<float>(M_PI)) / kBarPeriodMs);
  int16_t barW = barMinW + static_cast<int16_t>(barPhase * (barMaxW - barMinW));
  g_tft.fillRect(kW / 2 - barMaxW / 2, barY, barMaxW, 3, ST7735_BLACK);
  g_tft.fillRoundRect(kW / 2 - barW / 2, barY, barW, 3, 1, kColorAccentBlue);

  // Two staggered rings expanding outward from a point below the bar,
  // fading from accent blue to black as they grow -- confined to its own
  // region, well clear of the wordmark and bar above, so a plain
  // full-width clear each frame can't touch either.
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
    uint16_t color = lerpColor565(kColorAccentBlue, ST7735_BLACK, phase);
    g_tft.drawCircle(cx, cy, radius, color);
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

#ifdef ARDUINO
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

  /*
   * Dosing task's own boot gate (settings/scale/display ready) typically
   * clears well under a second in -- long before the splash's wordmark
   * and ripples have had any time to actually be seen -- and its first
   * real command overwrites this single-slot mailbox immediately.
   * Holding BOOT on screen for a minimum stretch regardless of what's
   * already arrived is the only way the splash is ever visible rather
   * than just flashed past; correctness of the FSM itself doesn't
   * depend on this, only what gets drawn.
   */
  constexpr uint32_t kMinBootSplashMs = 1800;
  uint32_t bootSplashStartMs = millis();
  bool havePendingCmd = false;
  DisplayCommand pendingCmd{};

  // Applies one command as the screen's new state: mode-change side effects
  // (full clear, connection-indicator reset, screensaver backlight toggle),
  // the render itself, and updating `last`/`havePendingCmd`. Shared by the
  // live-command and flush-pending-after-boot branches below so the two
  // can't drift apart (e.g. one of them silently skipping the backlight
  // toggle, or leaving a stale pending command primed to replay later).
  auto applyCommand = [&](const DisplayCommand &cmd) {
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
    havePendingCmd = false;
  };

  const TickType_t frameFloor = pdMS_TO_TICKS(16);  // ~60fps frame-rate floor.
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    DisplayCommand cmd;
    bool holdingBoot =
        last.mode == DisplayMode::BOOT && (millis() - bootSplashStartMs) < kMinBootSplashMs;

    // Block up to the frame floor for a new command; this both rate-limits
    // needless redraw work and lets the task idle when nothing changed.
    if (xQueueReceive(g_display_mailbox, &cmd, frameFloor) == pdTRUE) {
      if (holdingBoot && cmd.mode != DisplayMode::BOOT) {
        // Keep the freshest real command waiting rather than the first
        // one seen -- the mailbox itself is single-slot, so an early
        // "IDLE, 0.0g" would otherwise go stale under a later one.
        pendingCmd = cmd;
        havePendingCmd = true;
      } else {
        applyCommand(cmd);
      }
    } else if (havePendingCmd && !holdingBoot) {
      applyCommand(pendingCmd);
    } else if (last.mode == DisplayMode::BOOT || last.mode == DisplayMode::OTA_UPDATE) {
      // These two modes animate continuously (boot splash, OTA liquid
      // fill) rather than only redrawing when new state arrives -- every
      // other mode is static between real state changes, so it's cheap
      // for this branch to just re-render with whatever was last sent.
      renderMode(last, false);
    }

    vTaskDelayUntil(&lastFrame, frameFloor);
  }
}
#endif  // ARDUINO

}  // namespace

#ifdef ARDUINO
void createDisplayTask() {
  xTaskCreatePinnedToCore(displayTaskFn, "Display",
                           TaskConfig::kDisplayStackBytes, nullptr,
                           TaskConfig::kDisplayPriority, nullptr,
                           TaskConfig::kDisplayCore);
}
#else
#include "DisplaySimMain.h"
#endif
