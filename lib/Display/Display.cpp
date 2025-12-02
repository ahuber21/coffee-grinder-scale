#include "Display.h"

TextColor colors = {ST7735_WHITE, ST7735_BLACK};
TextColor colorTop = colors;
TextColor colorMain = colors;
TextColor colorBottom = colors;

Display::Display(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t ss,
                 uint8_t dc, uint8_t cs, uint8_t reset, uint8_t backlight)
    : m_spiDisplay(HSPI), m_display(&m_spiDisplay, cs, dc, reset), m_sck(sck),
      m_miso(miso), m_mosi(mosi), m_ss(ss), m_backlightPin(backlight),
      m_fps(16), m_turned_on(false), m_forceRefresh(false) {
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

void Display::refresh() {
  m_forceRefresh = true;
}

void Display::clear() {
  m_display.fillScreen(ST7735_BLACK);
  memset(m_lastDisplayRefreshMillis, 0, sizeof(uint32_t) * VA_MAX);
  m_forceRefresh = true;
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

  static uint16_t lastGrindingCurrentColorForFPS = 0xFFFF;
  static char lastGrindingCurrentStr[16] = "";
  static char lastGrindingTargetStr[16] = "";
  static char lastGrindingTimeStr[16] = "";
  static uint16_t lastGrindingCurrentColor = 0xFFFF;
  static uint16_t lastGrindingTargetColor = 0xFFFF;
  static uint16_t lastGrindingTimeColor = 0xFFFF;

  if (m_forceRefresh) {
    lastGrindingCurrentStr[0] = '\0';
    lastGrindingTargetStr[0] = '\0';
    lastGrindingTimeStr[0] = '\0';
    m_forceRefresh = false;
  }

  const uint32_t now = millis();
  const bool colorChangedForFPS = (currentColor != lastGrindingCurrentColorForFPS);

  // Format strings with fixed width to ensure full overwrite
  char currentStr[16];
  char targetStr[16];
  char timeStr[16];

  float displayGrams = currentGrams;
  if (displayGrams > -0.05f && displayGrams < 0.05f) {
    displayGrams = 0.0f;
  }

  snprintf(currentStr, sizeof(currentStr), "%4.1f", displayGrams);
  snprintf(targetStr, sizeof(targetStr), "/%4.1f", targetGrams);
  snprintf(timeStr, sizeof(timeStr), "%4.1fs", seconds);

  const bool sameCurrent =
      (strcmp(currentStr, lastGrindingCurrentStr) == 0) && (currentColor == lastGrindingCurrentColor);
  const bool sameTarget =
      (strcmp(targetStr, lastGrindingTargetStr) == 0) && (targetColor == lastGrindingTargetColor);
  const bool sameTime =
      (strcmp(timeStr, lastGrindingTimeStr) == 0) && (timeColor == lastGrindingTimeColor);

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
  lastGrindingCurrentColorForFPS = currentColor;

  // Update last state
  strncpy(lastGrindingCurrentStr, currentStr, sizeof(lastGrindingCurrentStr));
  lastGrindingCurrentStr[sizeof(lastGrindingCurrentStr) - 1] = '\0';
  strncpy(lastGrindingTargetStr, targetStr, sizeof(lastGrindingTargetStr));
  lastGrindingTargetStr[sizeof(lastGrindingTargetStr) - 1] = '\0';
  strncpy(lastGrindingTimeStr, timeStr, sizeof(lastGrindingTimeStr));
  lastGrindingTimeStr[sizeof(lastGrindingTimeStr) - 1] = '\0';
  lastGrindingCurrentColor = currentColor;
  lastGrindingTargetColor = targetColor;
  lastGrindingTimeColor = timeColor;

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
  const uint16_t targetH = h;

  // Vertical layout
  // Current weight at top
  const int16_t currentX = (DISPLAY_WIDTH - currentW) / 2;
  const int16_t currentY = 20;

  m_display.setTextColor(currentColor, ST7735_BLACK);
  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(currentSize);
  m_display.print(currentStr);

  // Target below current
  const int16_t targetX = (DISPLAY_WIDTH - targetW) / 2;
  const int16_t targetY = currentY + currentH + 10;

  m_display.setCursor(targetX, targetY);
  m_display.setTextSize(targetSize);
  m_display.setTextColor(targetColor, ST7735_BLACK);
  m_display.print(targetStr);

  // Time string at bottom
  const uint8_t timeSize = 2;
  m_display.setTextSize(timeSize);
  m_display.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);

  const int16_t timeX = (DISPLAY_WIDTH - w) / 2;
  const int16_t timeY = DISPLAY_HEIGHT - h - 10;

  m_display.setCursor(timeX, timeY);
  m_display.setTextSize(timeSize);
  m_display.setTextColor(timeColor, ST7735_BLACK);
  m_display.print(timeStr);

  drawConnectionIndicator(connectionIndicatorColor);
}

void Display::displayIdleLayout(float currentGrams, uint16_t connectionIndicatorColor) {
  wakeUp();

  static char lastIdleCurrentStr[16] = "";
  static uint16_t lastIdleConnectionColor = 0xFFFF;

  if (m_forceRefresh) {
    lastIdleCurrentStr[0] = '\0';
    lastIdleConnectionColor = 0xFFFF;
    m_forceRefresh = false;
  }

  const uint32_t now = millis();

  // Format string with fixed width for stable overwrite
  char currentStr[16];
  float displayGrams = currentGrams;
  if (displayGrams > -0.05f && displayGrams < 0.05f) {
    displayGrams = 0.0f;
  }
  snprintf(currentStr, sizeof(currentStr), "%5.1f", displayGrams);

  const bool sameText = (strcmp(currentStr, lastIdleCurrentStr) == 0);
  const bool sameIndicator = (connectionIndicatorColor == lastIdleConnectionColor);

  // Skip if nothing visible changed
  if (sameText && sameIndicator) {
    return;
  }

  if (now - m_lastDisplayRefreshMillis[CENTER] < 1000.0f / m_fps) {
    return;
  }
  m_lastDisplayRefreshMillis[CENTER] = now;

  strncpy(lastIdleCurrentStr, currentStr, sizeof(lastIdleCurrentStr));
  lastIdleCurrentStr[sizeof(lastIdleCurrentStr) - 1] = '\0';
  lastIdleConnectionColor = connectionIndicatorColor;

  const uint8_t currentSize = 2; // Reduced to 2 to fit 80px width

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
