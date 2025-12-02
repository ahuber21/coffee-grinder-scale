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
  static int16_t lastGrindingLayoutX = -1;
  static int16_t lastGrindingLayoutWidth = -1;

  if (m_forceRefresh) {
    lastGrindingCurrentStr[0] = '\0';
    lastGrindingTargetStr[0] = '\0';
    lastGrindingTimeStr[0] = '\0';
    lastGrindingLayoutX = -1;
    lastGrindingLayoutWidth = -1;
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
  const uint8_t targetSize = 2;

  int16_t x, y;
  uint16_t w, h;

  // Vertical layout
  // Current weight at top
  // Parse parts
  long totalDecigrams = (long)round(abs(displayGrams) * 10);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;
  bool isNegative = displayGrams < -0.05f;

  char intStr[10];
  itoa(intPart, intStr, 10);
  char decStr[5];
  itoa(decPart, decStr, 10);

  // Sizes
  const uint8_t intSize = 4;
  const uint8_t decSize = 2;
  const uint8_t minusSize = 2;
  const uint8_t dotSize = 2;

  // Calculate width (monospaced assumption: 6 * size)
  int16_t intWidth = strlen(intStr) * 6 * intSize;
  int16_t decWidth = strlen(decStr) * 6 * decSize;
  int16_t minusWidth = isNegative ? (6 * minusSize) : 0;
  int16_t dotWidth = 6 * dotSize;
  int16_t totalWidth = minusWidth + intWidth + dotWidth + decWidth;

  const int16_t currentY = 38;
  int16_t currentX = (DISPLAY_WIDTH - totalWidth) / 2;

  // Clear area for current weight (height 32 for size 4) only if layout changed
  if (currentX != lastGrindingLayoutX || totalWidth != lastGrindingLayoutWidth) {
    m_display.fillRect(0, currentY, DISPLAY_WIDTH, 32, ST7735_BLACK);
  }
  lastGrindingLayoutX = currentX;
  lastGrindingLayoutWidth = totalWidth;

  m_display.setTextColor(currentColor, ST7735_BLACK);

  if (isNegative) {
    m_display.setCursor(currentX, currentY + 8); // Middle align roughly
    m_display.setTextSize(minusSize);
    m_display.print("-");
    currentX += minusWidth;
  }

  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(intSize);
  m_display.print(intStr);
  currentX += intWidth;

  m_display.setCursor(currentX, currentY + 16); // Bottom align
  m_display.setTextSize(dotSize);
  m_display.print(".");
  currentX += dotWidth;

  m_display.setCursor(currentX, currentY + 16); // Bottom align
  m_display.setTextSize(decSize);
  m_display.print(decStr);

  // Target below current
  m_display.setTextSize(targetSize);
  m_display.getTextBounds(targetStr, 0, 0, &x, &y, &w, &h);
  const uint16_t targetW = w;
  const uint16_t targetH = h;

  const int16_t targetX = (DISPLAY_WIDTH - targetW) / 2;
  const int16_t targetY = currentY + 32 + 10; // 32 is height of int part

  m_display.setCursor(targetX, targetY);
  m_display.setTextSize(targetSize);
  m_display.setTextColor(targetColor, ST7735_BLACK);
  m_display.print(targetStr);

  // Time string at bottom
  const uint8_t timeSize = 2;
  m_display.setTextSize(timeSize);
  m_display.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);

  const int16_t timeX = (DISPLAY_WIDTH - w) / 2;
  const int16_t timeY = targetY + targetH + 20;

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
  static int16_t lastIntStartX = -1;
  static bool lastIsNegative = false;

  if (m_forceRefresh) {
    lastIdleCurrentStr[0] = '\0';
    lastIdleConnectionColor = 0xFFFF;
    lastIntStartX = -1;
    lastIsNegative = false;
    m_forceRefresh = false;
  }

  const uint32_t now = millis();

  // Check for out of range
  if (abs(currentGrams) > 99.95f) {
    if (strcmp("MAX", lastIdleCurrentStr) == 0 && connectionIndicatorColor == lastIdleConnectionColor) {
      return;
    }

    if (now - m_lastDisplayRefreshMillis[CENTER] < 1000.0f / m_fps) {
      return;
    }
    m_lastDisplayRefreshMillis[CENTER] = now;

    strncpy(lastIdleCurrentStr, "MAX", sizeof(lastIdleCurrentStr));
    lastIdleConnectionColor = connectionIndicatorColor;

    m_display.fillRect(0, (DISPLAY_HEIGHT - 32) / 2, DISPLAY_WIDTH, 32, ST7735_BLACK);
    m_display.setTextColor(ST7735_WHITE, ST7735_BLACK);

    const char* text = "MAX";
    m_display.setTextSize(3);
    int16_t x, y;
    uint16_t w, h;
    m_display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
    m_display.setCursor((DISPLAY_WIDTH - w) / 2, (DISPLAY_HEIGHT - h) / 2);
    m_display.print(text);

    drawConnectionIndicator(connectionIndicatorColor);
    return;
  }

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

  bool wasMax = (strcmp(lastIdleCurrentStr, "MAX") == 0);
  strncpy(lastIdleCurrentStr, currentStr, sizeof(lastIdleCurrentStr));
  lastIdleCurrentStr[sizeof(lastIdleCurrentStr) - 1] = '\0';
  lastIdleConnectionColor = connectionIndicatorColor;

  // Parse parts
  long totalDecigrams = (long)round(abs(displayGrams) * 10);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;
  bool isNegative = displayGrams < -0.05f;

  char intStr[10];
  itoa(intPart, intStr, 10);
  char decStr[5];
  itoa(decPart, decStr, 10);

  // Sizes
  const uint8_t intSize = 4;
  const uint8_t decSize = 2;
  const uint8_t minusSize = 2;
  const uint8_t dotSize = 1;

  const int16_t currentY = (DISPLAY_HEIGHT - 32) / 2; // 32 is height of int part

  // Layout: Anchor dot at x = 60
  // Integer part ends at 60
  int16_t intWidth = strlen(intStr) * 6 * intSize;
  int16_t intStartX = 60 - intWidth;

  // Handle clearing
  if (wasMax) {
    m_display.fillRect(0, currentY, DISPLAY_WIDTH, 32, ST7735_BLACK);
    lastIntStartX = -1;
  } else if (lastIntStartX != -1) {
    // Clear shrinking integer part
    if (intStartX > lastIntStartX) {
      m_display.fillRect(lastIntStartX, currentY, intStartX - lastIntStartX, 32, ST7735_BLACK);
    }
    // Clear removed minus sign
    if (lastIsNegative && !isNegative) {
      m_display.fillRect(0, currentY + 8, 6 * minusSize, 8 * minusSize, ST7735_BLACK);
    }
  }

  lastIntStartX = intStartX;
  lastIsNegative = isNegative;

  m_display.setTextColor(ST7735_WHITE, ST7735_BLACK);

  if (isNegative) {
    // Minus sign fixed at x=0
    m_display.setCursor(0, currentY + 8);
    m_display.setTextSize(minusSize);
    m_display.print("-");
  }

  m_display.setCursor(intStartX, currentY);
  m_display.setTextSize(intSize);
  m_display.print(intStr);

  // Dot
  m_display.setCursor(60, currentY + 21); // Bottom align
  m_display.setTextSize(dotSize);
  m_display.print(".");

  // Decimal
  m_display.setCursor(60 + (6 * dotSize), currentY + 14); // Bottom align
  m_display.setTextSize(decSize);
  m_display.print(decStr);

  drawConnectionIndicator(connectionIndicatorColor);
}

void Display::displayConfirmLayout(float targetGrams) {
  wakeUp();

  static float lastTargetGrams = -1.0f;
  static int16_t lastConfirmLayoutX = -1;
  static int16_t lastConfirmLayoutWidth = -1;

  if (m_forceRefresh) {
    lastTargetGrams = -1.0f;
    lastConfirmLayoutX = -1;
    lastConfirmLayoutWidth = -1;
    m_forceRefresh = false;
  }

  // Check if anything changed
  if (lastTargetGrams == targetGrams) {
    return;
  }
  lastTargetGrams = targetGrams;

  // Clear screen once when value changes (or on first run)
  // Since this is a full screen layout change usually, we might want to clear everything if we are coming from another state
  // But here we assume we are in the loop.
  // To be safe against artifacts from previous screens if not cleared externally:
  // The caller usually handles state transitions.
  // We will just clear the area we draw on, or the whole screen if we want to be sure.
  // Given the user complained about flicker, we should avoid full clear if possible,
  // but since the value doesn't change often in confirm screen (it's static target),
  // we can afford a full clear on first draw, and then nothing.

  // However, to support the "large digits" style which has variable width, we need the smart clear logic.

  // Parse parts
  long totalDecigrams = (long)round(abs(targetGrams) * 10);
  int intPart = totalDecigrams / 10;
  int decPart = totalDecigrams % 10;

  char intStr[10];
  itoa(intPart, intStr, 10);
  char decStr[5];
  itoa(decPart, decStr, 10);

  // Sizes
  const uint8_t intSize = 4;
  const uint8_t decSize = 2;
  const uint8_t dotSize = 2;

  // Calculate width
  int16_t intWidth = strlen(intStr) * 6 * intSize;
  int16_t decWidth = strlen(decStr) * 6 * decSize;
  int16_t dotWidth = 6 * dotSize;
  int16_t totalWidth = intWidth + dotWidth + decWidth;

  // Layout
  // Weight at top
  int16_t weightY = 38;
  int16_t weightX = (DISPLAY_WIDTH - totalWidth) / 2;

  // "OK?" below
  const char* title = "OK?";
  const uint8_t titleSize = 3;
  int16_t x, y;
  uint16_t w, h;
  m_display.setTextSize(titleSize);
  m_display.getTextBounds(title, 0, 0, &x, &y, &w, &h);
  int16_t titleX = (DISPLAY_WIDTH - w) / 2;
  int16_t titleY = weightY + 32 + 20;

  // Clear screen (simplest way to ensure clean slate for this static screen)
  // Since this function returns early if value hasn't changed, this clear only happens once per value change.
  m_display.fillScreen(ST7735_BLACK);

  // Draw Weight
  m_display.setTextColor(ST7735_WHITE, ST7735_BLACK);
  m_display.setCursor(weightX, weightY);
  m_display.setTextSize(intSize);
  m_display.print(intStr);
  weightX += intWidth;

  m_display.setCursor(weightX, weightY + 16); // Bottom align
  m_display.setTextSize(dotSize);
  m_display.print(".");
  weightX += dotWidth;

  m_display.setCursor(weightX, weightY + 16); // Bottom align
  m_display.setTextSize(decSize);
  m_display.print(decStr);

  // Draw Title
  m_display.setCursor(titleX, titleY);
  m_display.setTextSize(titleSize);
  m_display.print(title);
}

void Display::drawConnectionIndicator(uint16_t color) {
  if (color != 0) {  // 0 means no indicator
    const int16_t x = 1;
    const int16_t y = DISPLAY_HEIGHT - 4;
    const int16_t radius = 3;
    m_display.fillCircle(x, y, radius, color);
  }
}
