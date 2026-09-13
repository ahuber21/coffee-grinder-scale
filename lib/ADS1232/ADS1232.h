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

// nop takes 2 cycles, plus 4 cycles to fetch next instruction
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

  // 1/2/64/128
  void setGain(uint8_t gain);

  // 10 or 80 sps
  void setSpeed(uint8_t sps);

  bool powerOn();
  void powerOff();

  // this is the internal calibration method of the ADC ,
  // not the calculation of the calFactor
  void calibrateADC();

  // tare the scale
  bool tare();

  // this is the number that will help us translate voltage
  // read from the ADC to units (grams,whatever).
  void setCalFactor(double cal);

  void setRingBufferSize(uint8_t datasetsize);

  // initialize the ring buffer with values
  void initRingBuffer();

  // get the raw ADC counts
  int32_t getRaw(bool &isStable);
  int32_t getRaw();

  // return the weight in grams, including calibration and tare
  double getUnits();

  void readADCIfReady();

 protected:
  void resetBuffer();

  bool isReady();  // checks the DOUT pin
  bool safeWait(uint32_t waitTime = 2000);
  void readADCWithWait();
  // gets and returns a single ADC value without any processing
  void readADC();

  // ADC pins
  uint8_t pdwnPin;   // keep HIGH for power on, LOW for power off
  uint8_t sclkPin;   // not regular SPI, read datasheet
  uint8_t doutPin;   // not regular SPI, read datasheet
  uint8_t spdPin;    // LOW = 10SPS , HIGH = 80SPS
  uint8_t gain1Pin;  // 0|0 = 1 , 0|1 = 2 , 1|0 = 64 , 1|1 = 128
  uint8_t gain0Pin;  // in our case, anything lower than 128 is not worth it.
                     // Keep both gain0/gain1 HIGH
  uint8_t tempPin;   // if HIGH, AINP/AINN can be used to measure temp from
                     // internal temp diodes (datasheet, page 13)

  // ADC config
  int32_t tareRaw;
  double calFactor;  // see SettingsSnapshot::calibration_factor for why this isn't float

  // ADC values
  uint8_t ringBufferIndex;
  uint8_t ringBufferSize;
  int32_t ringBuffer[RING_BUFFER_MAX_SIZE];
  // last filtered units value (grams) for delta limiting
  float _lastUnitsFiltered = NAN;
};
