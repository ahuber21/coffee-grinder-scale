#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

class Display {
public:
  // Constructor
  Display(int8_t sck, int8_t miso, int8_t mosi, int8_t ss, int8_t dc, int8_t cs,
          int8_t reset, int8_t backlight);

  // Initialize the display
  void begin();

  // Display "Hello, World!" on the screen
  void helloWorld();

  // Clear the screen
  void clearScreen();

private:
  // Create an instance of the Adafruit_ST7735 class
  SPIClass spi;
  Adafruit_ST7735 tft;

  int8_t sckPin;
  int8_t misoPin;
  int8_t mosiPin;
  int8_t ssPin;
  int8_t backlightPin;
};
