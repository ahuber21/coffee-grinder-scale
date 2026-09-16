#pragma once

/**
 * Native-build-only stand-ins for what DisplayTask.cpp otherwise gets from
 * Arduino/Adafruit_ST7735/FreeRTOS -- exists purely so the *same*
 * rendering code can run host-side for `tools/display_sim` (see that
 * directory's README). Nothing here is used by the real firmware.
 */

#include <cstdint>
#include <cstdlib>

#include "DisplaySimFont.h"

// The two colors this project's rendering code actually references by
// name (every other color is a local RGB565 constant already computed in
// DisplayTask.cpp) -- normally pulled in transitively from
// Adafruit_ST7735.h.
constexpr uint16_t ST7735_BLACK = 0x0000;
constexpr uint16_t ST7735_WHITE = 0xFFFF;

// millis(), settable from the simulator's own driver loop so a whole
// scripted scenario can be stepped through deterministically rather than
// tied to wall-clock time.
inline uint32_t &simMillisRef() {
  static uint32_t ms = 0;
  return ms;
}
inline uint32_t millis() { return simMillisRef(); }

// Arduino's random(min, max): a uniform long in [min, max). No need to
// match its exact PRNG -- nothing here is asserted against a specific
// sequence, only ever looked at visually.
inline long random(long minVal, long maxVal) {
  if (maxVal <= minVal) return minVal;
  return minVal + (std::rand() % (maxVal - minVal));
}

/**
 * In-memory RGB565 framebuffer standing in for `Adafruit_ST7735`. Method
 * surface is exactly what DisplayTask.cpp's rendering code calls on
 * `g_tft` -- see that file's `grep -oE "g_tft\.[a-zA-Z_]+"` for the
 * authoritative list if this ever needs extending. Text rendering reuses
 * the real Adafruit_GFX font table/layout algorithm (see
 * DisplaySimFont.h) so measured/rendered text matches the physical panel.
 */
class FakeCanvas {
 public:
  static constexpr int kWidth = 80;
  static constexpr int kHeight = 160;
  uint16_t pixels[kHeight][kWidth] = {};

  void fillScreen(uint16_t color) { fillRect(0, 0, kWidth, kHeight, color); }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    int16_t x0 = x < 0 ? 0 : x;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t x1 = x + w > kWidth ? static_cast<int16_t>(kWidth) : static_cast<int16_t>(x + w);
    int16_t y1 = y + h > kHeight ? static_cast<int16_t>(kHeight) : static_cast<int16_t>(y + h);
    for (int16_t yy = y0; yy < y1; ++yy) {
      for (int16_t xx = x0; xx < x1; ++xx) pixels[yy][xx] = color;
    }
  }

  void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
    for (int16_t yy = -r; yy <= r; ++yy) {
      for (int16_t xx = -r; xx <= r; ++xx) {
        if (xx * xx + yy * yy <= r * r) setPixel(cx + xx, cy + yy, color);
      }
    }
  }

  // Midpoint circle -- only ever used for the boot splash's rings and the
  // connection-indicator dot outline, neither measured, just watched.
  void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
    int16_t f = 1 - r, ddFx = 1, ddFy = -2 * r, x = 0, y = r;
    setPixel(cx, cy + r, color);
    setPixel(cx, cy - r, color);
    setPixel(cx + r, cy, color);
    setPixel(cx - r, cy, color);
    while (x < y) {
      if (f >= 0) {
        y--;
        ddFy += 2;
        f += ddFy;
      }
      x++;
      ddFx += 2;
      f += ddFx;
      setPixel(cx + x, cy + y, color);
      setPixel(cx - x, cy + y, color);
      setPixel(cx + x, cy - y, color);
      setPixel(cx - x, cy - y, color);
      setPixel(cx + y, cy + x, color);
      setPixel(cx - y, cy + x, color);
      setPixel(cx + y, cy - x, color);
      setPixel(cx - y, cy - x, color);
    }
  }

  // Only ever called with a tiny radius (a 2px accent underline) -- a
  // plain rect is visually indistinguishable there, not worth a faithful
  // corner-rounding implementation for this tool.
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t /*r*/,
                      uint16_t color) {
    fillRect(x, y, w, h, color);
  }

  void setTextSize(uint8_t s) { _size = s > 0 ? s : 1; }
  void setTextColor(uint16_t color) {
    _fg = color;
    _bg = color;
  }
  void setTextColor(uint16_t fg, uint16_t bg) {
    _fg = fg;
    _bg = bg;
  }
  void setCursor(int16_t x, int16_t y) {
    _cx = x;
    _cy = y;
  }

  void print(const char *s) {
    int16_t x = _cx;
    for (const char *p = s; *p; ++p) {
      drawChar(x, _cy, static_cast<unsigned char>(*p));
      x = static_cast<int16_t>(x + 6 * _size);
    }
    _cx = x;
  }

  // Mirrors Adafruit_GFX::getTextBounds' classic-font (non-gfxFont)
  // branch: every char advances x by 6*size regardless of glyph
  // content, single line (this project never prints a string containing
  // '\n', so that branch isn't reproduced here).
  void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1,
                      uint16_t *w, uint16_t *h) {
    int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
    int16_t cx = x, cy = y;
    for (const char *p = str; *p; ++p) {
      int16_t x2 = static_cast<int16_t>(cx + 6 * _size - 1);
      int16_t y2 = static_cast<int16_t>(cy + 8 * _size - 1);
      if (x2 > maxx) maxx = x2;
      if (y2 > maxy) maxy = y2;
      if (cx < minx) minx = cx;
      if (cy < miny) miny = cy;
      cx = static_cast<int16_t>(cx + 6 * _size);
    }
    *x1 = (minx == 0x7FFF) ? x : minx;
    *y1 = (miny == 0x7FFF) ? y : miny;
    *w = (maxx >= minx) ? static_cast<uint16_t>(maxx - minx + 1) : 0;
    *h = (maxy >= miny) ? static_cast<uint16_t>(maxy - miny + 1) : 0;
  }

 private:
  void setPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    pixels[y][x] = color;
  }

  // Reimplements Adafruit_GFX::drawChar's classic-font branch exactly
  // (same column/row bit order, same background-fill-only-if-opaque
  // rule, same trailing spacer column) against kFont5x7 instead of SPI.
  void drawChar(int16_t x, int16_t y, unsigned char c) {
    if (c >= 176) c++;  // 'classic' charset behavior, matches the real drawChar()
    for (int8_t i = 0; i < 5; ++i) {
      uint8_t line = kFont5x7[c * 5 + i];
      for (int8_t j = 0; j < 8; ++j, line = static_cast<uint8_t>(line >> 1)) {
        if (line & 1) {
          if (_size == 1) {
            setPixel(static_cast<int16_t>(x + i), static_cast<int16_t>(y + j), _fg);
          } else {
            fillRect(static_cast<int16_t>(x + i * _size), static_cast<int16_t>(y + j * _size),
                     _size, _size, _fg);
          }
        } else if (_bg != _fg) {
          if (_size == 1) {
            setPixel(static_cast<int16_t>(x + i), static_cast<int16_t>(y + j), _bg);
          } else {
            fillRect(static_cast<int16_t>(x + i * _size), static_cast<int16_t>(y + j * _size),
                     _size, _size, _bg);
          }
        }
      }
    }
    if (_bg != _fg) {
      fillRect(static_cast<int16_t>(x + 5 * _size), y, _size, static_cast<int16_t>(8 * _size),
                _bg);
    }
  }

  uint8_t _size = 1;
  uint16_t _fg = ST7735_WHITE;
  uint16_t _bg = ST7735_WHITE;
  int16_t _cx = 0;
  int16_t _cy = 0;
};
