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

  // Fix displaying -0.00 and show 0.00 instead
  if (strcmp(text, "-0.00") == 0) {
    text = "0.00";
  }

  int16_t x, y;
  uint16_t w, h;

  // Find appropriate text size
  uint8_t size = 2;
  m_display.setTextSize(size);
  m_display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  while (w > DISPLAY_WIDTH && size > 1) {
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

  case GRINDING_LAYOUT:
    return;

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
  uint32_t duty = 0xFFFFFFFFu / 100u * brightness;
  ledcWrite(1, duty);
}

void Display::displayGrindingLayout(float currentGrams, float targetGrams, float seconds,
                                    uint16_t currentColor, uint16_t targetColor,
                                    uint16_t timeColor, uint16_t connectionIndicatorColor) {
  wakeUp();

  // State for frame pacing and change detection
  static uint16_t lastCurrentColorForFPS = 0xFFFF;
  static char lastCurrentStr[16] = "";
  static char lastTargetStr[16] = "";
  static char lastTimeStr[16] = "";
  static uint16_t lastCurrentColor = 0xFFFF;
  static uint16_t lastTargetColor = 0xFFFF;
  static uint16_t lastTimeColor = 0xFFFF;

  const uint32_t now = millis();
  const bool colorChangedForFPS = (currentColor != lastCurrentColorForFPS);

  // Format strings with fixed width to ensure full overwrite
  char currentStr[16];
  char targetStr[16];
  char timeStr[16];

  float displayGrams = currentGrams;
  if (displayGrams > -0.05f && displayGrams < 0.05f) {
    displayGrams = 0.0f;
  }

  snprintf(currentStr, sizeof(currentStr), "%5.1f", displayGrams);
  snprintf(targetStr, sizeof(targetStr), "/%4.1f", targetGrams);
  snprintf(timeStr, sizeof(timeStr), "%5.1fs", seconds);

  const bool sameCurrent =
      (strcmp(currentStr, lastCurrentStr) == 0) && (currentColor == lastCurrentColor);
  const bool sameTarget =
      (strcmp(targetStr, lastTargetStr) == 0) && (targetColor == lastTargetColor);
  const bool sameTime =
      (strcmp(timeStr, lastTimeStr) == 0) && (timeColor == lastTimeColor);

  // Skip if nothing visible changed
  if (!colorChangedForFPS && sameCurrent && sameTarget && sameTime) {
    return;
  }

  // FPS limit if color is unchanged
  if (!colorChangedForFPS &&
      (now - m_lastDisplayRefreshMillis[GRINDING_LAYOUT] < 1000.0f / m_fps)) {
    return;
  }
  m_lastDisplayRefreshMillis[GRINDING_LAYOUT] = now;
  lastCurrentColorForFPS = currentColor;

  // Update last state
  strncpy(lastCurrentStr, currentStr, sizeof(lastCurrentStr));
  lastCurrentStr[sizeof(lastCurrentStr) - 1] = '\0';
  strncpy(lastTargetStr, targetStr, sizeof(lastTargetStr));
  lastTargetStr[sizeof(lastTargetStr) - 1] = '\0';
  strncpy(lastTimeStr, timeStr, sizeof(lastTimeStr));
  lastTimeStr[sizeof(lastTimeStr) - 1] = '\0';
  lastCurrentColor = currentColor;
  lastTargetColor = targetColor;
  lastTimeColor = timeColor;

  // Static font sizes (optimized for 0.0-99.9g range)
  const uint8_t currentSize = 3;
  const uint8_t targetSize = 2;

  int16_t x, y;
  uint16_t w, h;

  // Current weight bounds
  m_display.setTextSize(currentSize);
  m_display.getTextBounds(currentStr, 0, 0, &x, &y, &w, &h);
  const uint16_t currentW = w;
  const uint16_t currentH = h;

  // Target text bounds
  m_display.setTextSize(targetSize);
  m_display.getTextBounds(targetStr, 0, 0, &x, &y, &w, &h);
  const uint16_t targetW = w;

  // Horizontal centering as a unit
  const uint16_t totalWidth = currentW + targetW + 3;
  const int16_t currentX = (DISPLAY_WIDTH - totalWidth) / 2;
  const int16_t currentY = 5;

  m_display.setTextColor(currentColor, ST7735_BLACK);
  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(currentSize);
  m_display.print(currentStr);

  // Target right of current, baseline aligned
  const int16_t targetX = currentX + currentW + 3;
  const int16_t targetY = currentY + currentH - (targetSize * 8);

  m_display.setCursor(targetX, targetY);
  m_display.setTextSize(targetSize);
  m_display.setTextColor(targetColor, ST7735_BLACK);
  m_display.print(targetStr);

  // Time string
  const uint8_t timeSize = 2;
  m_display.setTextSize(timeSize);
  m_display.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);

  const int16_t timeX = (DISPLAY_WIDTH - w) / 2;
  const int16_t timeY = DISPLAY_HEIGHT - h - 2;

  m_display.setCursor(timeX, timeY);
  m_display.setTextSize(timeSize);
  m_display.setTextColor(timeColor, ST7735_BLACK);
  m_display.print(timeStr);

  drawConnectionIndicator(connectionIndicatorColor);
}

void Display::displayIdleLayout(float currentGrams, uint16_t connectionIndicatorColor) {
  wakeUp();

  static char lastCurrentStr[16] = "";
  static uint16_t lastConnectionColor = 0xFFFF;

  const uint32_t now = millis();

  // Format string with fixed width for stable overwrite
  char currentStr[16];
  float displayGrams = currentGrams;
  if (displayGrams > -0.05f && displayGrams < 0.05f) {
    displayGrams = 0.0f;
  }
  snprintf(currentStr, sizeof(currentStr), "%5.1f g", displayGrams);

  const bool sameText = (strcmp(currentStr, lastCurrentStr) == 0);
  const bool sameIndicator = (connectionIndicatorColor == lastConnectionColor);

  // Skip if nothing visible changed
  if (sameText && sameIndicator) {
    return;
  }

  if (now - m_lastDisplayRefreshMillis[CENTER] < 1000.0f / m_fps) {
    return;
  }
  m_lastDisplayRefreshMillis[CENTER] = now;

  strncpy(lastCurrentStr, currentStr, sizeof(lastCurrentStr));
  lastCurrentStr[sizeof(lastCurrentStr) - 1] = '\0';
  lastConnectionColor = connectionIndicatorColor;

  const uint8_t currentSize = 3; // Sized to fit negative weights without wrapping

  int16_t x, y;
  uint16_t w, h;
  m_display.setTextSize(currentSize);
  m_display.getTextBounds(currentStr, 0, 0, &x, &y, &w, &h);

  const int16_t currentX = (DISPLAY_WIDTH - w) / 2;
  const int16_t currentY = (DISPLAY_HEIGHT - h) / 2;

  m_display.setTextColor(ST7735_WHITE, ST7735_BLACK);
  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(currentSize);
  m_display.print(currentStr);

  drawConnectionIndicator(connectionIndicatorColor);
}

void Display::drawConnectionIndicator(uint16_t color) {
  if (color != 0) {  // 0 means no indicator
    const int16_t x = DISPLAY_WIDTH - 8;
    const int16_t y = 4;
    const int16_t radius = 3;
    m_display.fillCircle(x, y, radius, color);
  }
}
