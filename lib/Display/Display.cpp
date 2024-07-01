#include "Display.h"

TextColor colors = {ST7735_WHITE, ST7735_BLACK};
TextColor colorTop = colors;
TextColor colorMain = colors;
TextColor colorBottom = colors;

Display::Display(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t ss,
                 uint8_t dc, uint8_t cs, uint8_t reset, uint8_t backlight)
    : m_spiDisplay(HSPI), m_display(&m_spiDisplay, cs, dc, reset), m_sck(sck),
      m_miso(miso), m_mosi(mosi), m_ss(ss), m_backlightPin(backlight),
      m_fps(16), m_turned_on(false) {
  memset(m_lastDisplayRefreshMillis, 0, sizeof(uint32_t) * VA_MAX);
}

void Display::begin() {
  m_spiDisplay.begin(m_sck, m_miso, m_mosi, m_ss);
  m_display.initR(INITR_MINI160x80);
  m_display.invertDisplay(false);
  ledcAttachPin(m_backlightPin, 1);
  ledcSetup(1, 100, 8);
  wakeUp();
}

void Display::displayString(const String &text, VerticalAlignment alignment) {
  displayString(text.c_str(), alignment);
}

void Display::displayString(const char *text, VerticalAlignment alignment) {
  wakeUp();

  uint32_t now = millis();
  if (now - m_lastDisplayRefreshMillis[(int)alignment] < 1000.0f / m_fps) {
    return;
  }
  m_lastDisplayRefreshMillis[(int)alignment] = now;

  int16_t x, y;
  uint16_t w, h;

  // find appropriate text size
  uint8_t size = 2;
  m_display.setTextSize(size);
  m_display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  while (w > DISPLAY_WIDTH) {
    m_display.setTextSize(--size);
    m_display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  }

  switch (alignment) {
  case CENTER:
    y = DISPLAY_HEIGHT / 2 - h / 2;
    break;

  case TWO_ROW_TOP:
    y = 0;
    break;

  case TWO_ROW_BOTTOM:
    y = DISPLAY_HEIGHT - h;
    break;

  case THREE_ROW_TOP:
    y = 0;
    break;

  case THREE_ROW_CENTER:
    y = DISPLAY_HEIGHT / 2 - h / 2;
    break;

  case THREE_ROW_BOTTOM:
    y = DISPLAY_HEIGHT - h;
    break;

  default:
    break;
  }

  x = DISPLAY_WIDTH / 2 - w / 2;

  m_display.fillRect(0, y, DISPLAY_WIDTH, h, ST7735_BLACK);
  m_display.setCursor(x, y);
  m_display.println(text);
}

void Display::setRotation(uint8_t rotation) { m_display.setRotation(rotation); }

void Display::setTextColor(TextColor color) {
  m_display.setTextColor(color.foreground, color.background);
}

void Display::clear() {
  m_display.fillScreen(ST7735_BLACK);
  memset(m_lastDisplayRefreshMillis, 0, sizeof(uint32_t) * VA_MAX);
}

void Display::shutDown() {
  if (!m_turned_on) {
    return;
  }
  m_turned_on = false;
  clear();
  setBrightness(0);
}

void Display::wakeUp() {
  if (m_turned_on) {
    return;
  }
  m_turned_on = true;
  setBrightness(1);
  clear();
}

void Display::setBrightness(uint8_t brightness) {
  uint32_t duty = 0xFFFFFFFF / 100 * brightness;
  ledcWrite(1, duty);
}