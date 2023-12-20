#include "Display.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// Constructor
Display::Display(int8_t sck, int8_t miso, int8_t mosi, int8_t ss, int8_t dc,
                 int8_t cs, int8_t reset, int8_t backlight)
    // : spi(HSPI), tft(&spi, cs, dc, reset), sckPin(sck), misoPin(miso),
    : spi(HSPI), tft(&spi, 15, 26, 27), sckPin(sck), misoPin(miso),
      mosiPin(mosi), ssPin(ss), backlightPin(backlight) {}

// Initialize the display
void Display::begin() {
  ledcSetup(1, 100, 8);
  // spi.begin(sckPin, misoPin, mosiPin, ssPin);
  spi.begin(14, 12, 13, 15);
  ledcAttachPin(backlightPin, 1);

  tft.initR(INITR_MINI160x80);
  tft.setRotation(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(2);
  tft.fillScreen(ST7735_BLACK);
}

// Display "Hello, World!" on the screen
void Display::helloWorld() {
  tft.setCursor(10, 10);
  tft.println("Hello, World!");
}

// Clear the screen
void Display::clearScreen() { tft.fillScreen(ST7735_BLACK); }
