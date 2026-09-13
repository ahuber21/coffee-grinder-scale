#include "ADS1232.h"

#define SERIAL_IF
// #define DEBUG

#ifdef SERIAL_IF
#define LOG_PRINT(x) Serial.print(x)
#define LOG_PRINTLN(x) Serial.println(x)
#ifdef DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#endif
#else
#define LOG_PRINT(x)
#define LOG_PRINTLN(x)
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#endif

ADS1232::ADS1232(uint8_t pdwn, uint8_t sclk, uint8_t dout, uint8_t spd,
                 uint8_t gain1pin, uint8_t gain0pin)
    : pdwnPin(pdwn),
      sclkPin(sclk),
      doutPin(dout),
      spdPin(spd),
      gain1Pin(gain1pin),
      gain0Pin(gain0pin),
      calFactor(1.0),
      ringBufferIndex(0),
      ringBufferSize(1) {
  resetBuffer();
}

void ADS1232::resetBuffer() {
  memset(ringBuffer, 0, RING_BUFFER_MAX_SIZE * sizeof(ringBuffer[0]));
  ringBufferIndex = 0;
}

bool ADS1232::begin() {
  pinMode(pdwnPin, OUTPUT);
  pinMode(sclkPin, OUTPUT);
  pinMode(doutPin, INPUT_PULLUP);
  pinMode(spdPin, OUTPUT);
  pinMode(gain1Pin, OUTPUT);
  pinMode(gain0Pin, OUTPUT);

  digitalWrite(pdwnPin, HIGH);
  digitalWrite(sclkPin, LOW);
  digitalWrite(spdPin, HIGH);
  digitalWrite(gain1Pin, LOW);
  digitalWrite(gain0Pin, LOW);

  return powerOn();
}

bool ADS1232::isReady() { return digitalRead(doutPin) == LOW; }

bool ADS1232::safeWait(uint32_t waitTime) {
  // if you want to implement a more robust and sophisticated timeout, please
  // check out: https://github.com/HamidSaffari/ADS123X
  uint32_t elapsed;
  elapsed = millis();
  while (!isReady()) {
    if (millis() > elapsed + waitTime) {
      // timeout
      LOG_PRINTLN("Error while waiting for ADC");
      return false;
    }
  }
  return true;
}

bool ADS1232::powerOn() {
  // Power-Up Sequence
  digitalWrite(pdwnPin, LOW);
  delay(10);
  digitalWrite(pdwnPin, HIGH);
  delay(26);
  digitalWrite(pdwnPin, LOW);
  delay(26);
  digitalWrite(pdwnPin, HIGH);

  // extra delay + prepare clock
  delay(10);
  digitalWrite(sclkPin, LOW);
  delay(10);

  // Power-Down Mode
  digitalWrite(pdwnPin, LOW);
  delay(26);
  digitalWrite(pdwnPin, HIGH);
  delay(8);   // t13, Wake-up time after power-down mode; internal clock
  delay(55);  // t11, Data ready after exiting standy mode; speed=1

  digitalWrite(sclkPin, LOW);
  if (!safeWait()) {
    LOG_PRINTLN("Power on error");
    return false;
  }
  calibrateADC();

  return true;
}

void ADS1232::powerOff() {
  digitalWrite(pdwnPin, LOW);
  digitalWrite(sclkPin, HIGH);
}

void ADS1232::setGain(uint8_t gain)  // 1/2/64/128 , default 128
{
  uint8_t adcGain1;
  uint8_t adcGain0;
  switch (gain) {
    case 1:
      adcGain1 = 0;
      adcGain0 = 0;
      break;
    case 2:
      adcGain1 = 0;
      adcGain0 = 1;
      break;
    case 64:
      adcGain1 = 1;
      adcGain0 = 0;
      break;
    case 128:
      adcGain1 = 1;
      adcGain0 = 1;
      break;
  }

  if (gain0Pin > 0 && gain1Pin > 0) {
    digitalWrite(gain1Pin, adcGain1);
    digitalWrite(gain0Pin, adcGain0);
  }

  calibrateADC();
}

void ADS1232::setSpeed(uint8_t sps) {
  digitalWrite(spdPin, sps == 80);
  calibrateADC();
}

void ADS1232::setRingBufferSize(uint8_t size) {
  size = (size <= RING_BUFFER_MAX_SIZE) ? size : RING_BUFFER_MAX_SIZE;
  ringBufferSize = size;
}

void ADS1232::initRingBuffer() {
  resetBuffer();
  while (ringBufferIndex != ringBufferSize - 1) {
    readADCWithWait();
  }
}

void ADS1232::calibrateADC() {
  DEBUG_PRINTLN("ADC calibration init...");
  readADCWithWait();
  // readADC returns with 25th pulse completed, so immediately continue with
  // 26th
  DELAY_NS_100;
  digitalWrite(sclkPin, HIGH);  // 26th pulse
  DELAY_NS_100;
  digitalWrite(sclkPin, LOW);  // end of 26th
  // actual calibration begins... wait for dout = LOW
  if (!safeWait()) {
    // oops...time out !!!
    LOG_PRINTLN("ADC calibration error");
    return;
  }
  readADCWithWait();  // read once without saving
  DEBUG_PRINTLN("ADC calibration done...");
  // all done, ready to read again...
}

bool ADS1232::tare() {
  bool isStable = false;
  tareRaw = getRaw(isStable);
  return isStable;
}

void ADS1232::setCalFactor(double cal) { calFactor = cal; }

double ADS1232::getUnits() {
  bool isStable = false;
  // double throughout, not just on the return type -- computing the
  // multiplication itself in float would throw away calFactor's extra
  // precision right here regardless of what type carried it in.
  double raw = getRaw(isStable) - tareRaw;
  double units = raw * calFactor;

  // First run (or invalid last) always accept
  if (isnan(_lastUnitsFiltered)) {
    _lastUnitsFiltered = units;
    return units;
  }

  if (!isStable) {
    // Apply glitch rejection only when buffer indicates instability
    constexpr float MAX_DELTA = 200.0f;  // grams
    float delta = units - _lastUnitsFiltered;
    if (delta > MAX_DELTA || delta < -MAX_DELTA) {
      // Treat as transient glitch: do NOT update last filtered value, just
      // return the previous stable reading.
      return _lastUnitsFiltered;
    }
  }

  _lastUnitsFiltered = units;
  return units;
}

// parameter isStable indicates that the returned value is an average over the
// entire ring buffer
int32_t ADS1232::getRaw(bool &isStable) {
  // last written ringBufferIndex is stored in ringBufferIndex
  int32_t rawSum = 0;
  for (uint8_t i = 0; i < ringBufferSize; ++i) {
    rawSum += ringBuffer[(ringBufferIndex + i) % ringBufferSize];
  }

  int32_t rawMean = rawSum / ringBufferSize;

  // if the values deviate by too much from the rawMean, we only return the
  // last measurement
  bool changing = false;
  for (uint8_t i = 0; i < ringBufferSize; ++i) {
    constexpr float maxRelDeviation = 0.0002f;
    const float maxDelta = maxRelDeviation * rawMean;
    int32_t currentValue = ringBuffer[(ringBufferIndex + i) % ringBufferSize];
    changing |= (maxDelta < abs(rawMean - currentValue));
  }

  isStable = !changing;
  return changing ? ringBuffer[ringBufferIndex % ringBufferSize] : rawMean;
}

int32_t ADS1232::getRaw() {
  bool stable = false;
  return getRaw(stable);
}

void ADS1232::readADCIfReady() {
  if (!isReady()) {
    return;
  }

  readADC();
}

void ADS1232::readADCWithWait() {
  safeWait();
  readADCIfReady();
}

static portMUX_TYPE s_adcMux = portMUX_INITIALIZER_UNLOCKED;

void ADS1232::readADC() {
  int32_t adcValue = 0;

  portENTER_CRITICAL(&s_adcMux);
  for (int i = 0; i < 24; i++) {
    digitalWrite(sclkPin, HIGH);
    // DELAY_NS_100;
    adcValue = (adcValue << 1) + digitalRead(doutPin);
    digitalWrite(sclkPin, LOW);
    // DELAY_NS_100;
  }
  digitalWrite(sclkPin, HIGH);  // keep dout high // 25th pulse
  // DELAY_NS_100;
  digitalWrite(sclkPin, LOW);

  portEXIT_CRITICAL(&s_adcMux);

  // this makes sure that the high 8 bits are zeroed
  adcValue = (adcValue << 8) / 256;

  ringBufferIndex = (++ringBufferIndex) % ringBufferSize;
  ringBuffer[ringBufferIndex] = adcValue;
}
