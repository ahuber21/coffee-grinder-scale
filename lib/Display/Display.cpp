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

  // fix displaying -0.00 and show 0.00 instead
  if (strcmp(text, "-0.00") == 0) {
    text = "0.00";
  }

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

  case GRINDING_LAYOUT:
    // This case is handled by displayGrindingLayout(), not here
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
  uint32_t duty = 0xFFFFFFFF / 100 * brightness;
  ledcWrite(1, duty);
}

void Display::displayGrindingLayout(float currentGrams, float targetGrams, float seconds,
                                   uint16_t currentColor, uint16_t targetColor, uint16_t timeColor) {
  wakeUp();

  // Check if colors changed to force immediate refresh
  static uint16_t lastCurrentColorForFPS = 0xFFFF;
  bool colorChanged = (currentColor != lastCurrentColorForFPS);

  uint32_t now = millis();
  if (!colorChanged && (now - m_lastDisplayRefreshMillis[GRINDING_LAYOUT] < 1000.0f / m_fps)) {
    return;
  }
  m_lastDisplayRefreshMillis[GRINDING_LAYOUT] = now;
  lastCurrentColorForFPS = currentColor;

  // Clear screen when content or colors change significantly
  static float lastCurrentGrams = -999.0f;
  static float lastTargetGrams = -999.0f;
  static uint16_t lastCurrentColor = 0xFFFF; // Invalid color to force first refresh
  if (abs(currentGrams - lastCurrentGrams) > 0.05f ||
      abs(targetGrams - lastTargetGrams) > 0.5f ||
      currentColor != lastCurrentColor) {
    m_display.fillScreen(ST7735_BLACK);
    lastCurrentGrams = currentGrams;
    lastTargetGrams = targetGrams;
    lastCurrentColor = currentColor;
  }

  // Format strings - fix negative zero display
  char currentStr[16], targetStr[16], timeStr[16];
  float displayGrams = currentGrams;
  if (displayGrams < 0.05 && displayGrams > -0.05) {
    displayGrams = 0.0; // Avoid -0.0 display
  }
  sprintf(currentStr, "%.1f", displayGrams);
  sprintf(targetStr, "/%.0fg", targetGrams);
  sprintf(timeStr, "%.1fs", seconds);

  // --- STATIC FONT SIZES (optimized for 0.0-99.9 range) ---
  uint8_t currentSize = 3; // Size 3 works well for up to "99.9"
  uint8_t targetSize = 2;  // Size 2 for target "/36g"

  int16_t x, y;
  uint16_t w, h;

  // --- DRAW CURRENT WEIGHT ---
  m_display.setTextSize(currentSize);
  m_display.getTextBounds(currentStr, 0, 0, &x, &y, &w, &h);
  uint16_t currentW = w;
  uint16_t currentH = h;

  // Get target text dimensions
  m_display.setTextSize(targetSize);
  m_display.getTextBounds(targetStr, 0, 0, &x, &y, &w, &h);
  uint16_t targetW = w;

  // Center both texts together as a unit
  uint16_t totalWidth = currentW + targetW + 3; // Total width including gap
  int16_t currentX = (DISPLAY_WIDTH - totalWidth) / 2;
  int16_t currentY = 5; // Small top margin

  m_display.setTextColor(currentColor, ST7735_BLACK);
  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(currentSize);
  m_display.print(currentStr);

  // --- DRAW TARGET SUFFIX ---

  // Position target text right after current weight, aligned to bottom
  int16_t targetX = currentX + currentW + 3; // Right after current + small gap
  int16_t targetY = currentY + currentH - (targetSize * 8); // Align to bottom of current weight

  m_display.setCursor(targetX, targetY);
  m_display.setTextSize(targetSize);
  m_display.setTextColor(targetColor, ST7735_BLACK);
  m_display.print(targetStr);

  // --- TIME (bottom of screen, more readable) ---
  uint8_t timeSize = 2; // Increased from 1 to 2 for better readability
  m_display.setTextSize(timeSize);
  m_display.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);

  int16_t timeX = (DISPLAY_WIDTH - w) / 2;
  int16_t timeY = DISPLAY_HEIGHT - h - 2; // Bottom margin

  m_display.setCursor(timeX, timeY);
  m_display.setTextSize(timeSize);
  m_display.setTextColor(timeColor, ST7735_BLACK);
  m_display.print(timeStr);
}

void Display::displayIdleLayout(float currentGrams) {
  wakeUp();

  uint32_t now = millis();
  if (now - m_lastDisplayRefreshMillis[CENTER] < 1000.0f / m_fps) {
    return;
  }
  m_lastDisplayRefreshMillis[CENTER] = now;

  // Clear screen only if content changed significantly
  static float lastDisplayedGrams = -999.0f;
  if (abs(currentGrams - lastDisplayedGrams) > 0.05f) {
    m_display.fillScreen(ST7735_BLACK);
    lastDisplayedGrams = currentGrams;
  }

  // Format string - fix negative zero display
  char currentStr[16];
  float displayGrams = currentGrams;
  if (displayGrams < 0.05 && displayGrams > -0.05) {
    displayGrams = 0.0; // Avoid -0.0 display
  }
  sprintf(currentStr, "%.1f g", displayGrams);

  // Use large font size for idle display (reduced to fit negative weights)
  uint8_t currentSize = 3; // Reduced from 4 to prevent line wrapping with negative weights

  int16_t x, y;
  uint16_t w, h;
  m_display.setTextSize(currentSize);
  m_display.getTextBounds(currentStr, 0, 0, &x, &y, &w, &h);

  // Center on screen
  int16_t currentX = (DISPLAY_WIDTH - w) / 2;
  int16_t currentY = (DISPLAY_HEIGHT - h) / 2;

  m_display.setTextColor(ST7735_WHITE, ST7735_BLACK);
  m_display.setCursor(currentX, currentY);
  m_display.setTextSize(currentSize);
  m_display.print(currentStr);
}