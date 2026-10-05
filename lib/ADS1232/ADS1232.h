/*
  ADS1232.h - library for TI ADS1232
  Ring-buffered ADS1232 library by Andreas Huber.

  Forked from John Sartzetakis, orginally released Jan 2019
  Released into the public domain.
  https://gitlab.com/jousis/ads1232-library

  ADS1232
  http://www.ti.com/lit/ds/symlink/ads1232.pdf
  24-Bit ADC

  Smoothing data functions taken from HX711_ADC by Olav Kallhovd
  https://github.com/olkal/HX711_ADC

  Also check out ADS123X library by Hamid Saffari
  https://github.com/HamidSaffari/ADS123X
*/

#pragma once

#include <Arduino.h>

// ~100ns of nops: each takes 2 cycles, plus 4 to fetch the next instruction.
#if defined(F_CPU) && (F_CPU == 80000000L)
#define DELAY_NS_100           \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop");
#endif
#if defined(F_CPU) && (F_CPU == 160000000L)
#define DELAY_NS_100           \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop");
#endif
#if defined(F_CPU) && (F_CPU == 240000000L)
#define DELAY_NS_100           \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop"); \
  __asm__ __volatile__("nop");
#endif

#define RING_BUFFER_MAX_SIZE 96
class ADS1232 {
 public:
  ADS1232(uint8_t pdwn, uint8_t sclk, uint8_t dout, uint8_t spd,
          uint8_t gain1pin, uint8_t gain0pin);

  bool begin();

  /** Sets the gain: 1, 2, 64 or 128. Any other value is ignored. */
  void setGain(uint8_t gain);

  /** Sets the conversion rate: 10 or 80 samples per second. */
  void setSpeed(uint8_t sps);

  bool powerOn();
  void powerOff();

  /** Runs the ADC's internal offset calibration (not the scale calibration factor). */
  void calibrateADC();

  /** Zeroes the scale at the current reading; true only if that reading was stable. */
  bool tare();

  /** Sets the factor that converts raw counts to units (grams). */
  void setCalFactor(double cal);

  /** Sets how many samples the ring buffer averages (1 to RING_BUFFER_MAX_SIZE). */
  void setRingBufferSize(uint8_t datasetsize);

  /** Fills the ring buffer with fresh readings. */
  void initRingBuffer();

  /** Raw ADC counts: the ring buffer's mean, or the latest sample while it is unsettled. */
  int32_t getRaw(bool &isStable);
  int32_t getRaw();

  /** The weight in grams, including calibration and tare. */
  double getUnits();

  /** Reads a conversion if one is ready, without waiting. */
  void readADCIfReady();

 protected:
  void resetBuffer();

  bool isReady();  ///< DOUT is low when a conversion is ready.
  bool safeWait(uint32_t waitTime = 2000);
  void readADCWithWait();
  /** Shifts in one conversion and stores it in the ring buffer. */
  void readADC();

  // ADC pins
  uint8_t pdwnPin;   ///< HIGH = power on, LOW = power off.
  uint8_t sclkPin;   ///< Not regular SPI; see the datasheet.
  uint8_t doutPin;   ///< Not regular SPI; see the datasheet.
  uint8_t spdPin;    ///< LOW = 10 SPS, HIGH = 80 SPS.
  uint8_t gain1Pin;  ///< With gain0Pin: 0|0 = 1, 0|1 = 2, 1|0 = 64, 1|1 = 128.
  uint8_t gain0Pin;

  // ADC config
  int32_t tareRaw;
  double calFactor;  ///< Double, since a float would lose precision multiplying large raw counts.

  // ADC values
  uint8_t ringBufferIndex;  ///< Slot of the most recent sample.
  uint8_t ringBufferSize;
  int32_t ringBuffer[RING_BUFFER_MAX_SIZE];
  float _lastUnitsFiltered = NAN;  ///< Previous reading, for glitch rejection in getUnits().
};
