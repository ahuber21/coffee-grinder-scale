#include "ADS1232.h"

#define LOG_PRINTLN(x) Serial.println(x)

ADS1232::ADS1232(uint8_t pdwn, uint8_t sclk, uint8_t dout, uint8_t spd,
                 uint8_t gain1pin, uint8_t gain0pin)
    : pdwnPin(pdwn),
      sclkPin(sclk),
      doutPin(dout),
      spdPin(spd),
      gain1Pin(gain1pin),
      gain0Pin(gain0pin),
      tareRaw(0),
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
  uint32_t start = millis();
  while (!isReady()) {
    if (millis() > start + waitTime) {
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

void ADS1232::setGain(uint8_t gain) {
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
    default:
      return;  // Not a hardware gain; leave the pins as they are.
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
  if (size > RING_BUFFER_MAX_SIZE) size = RING_BUFFER_MAX_SIZE;
  ringBufferSize = size > 0 ? size : 1;
}

void ADS1232::initRingBuffer() {
  resetBuffer();
  while (ringBufferIndex != ringBufferSize - 1) {
    readADCWithWait();
  }
}

void ADS1232::calibrateADC() {
  readADCWithWait();
  // readADC() returns after the 25th pulse, so the 26th starts the calibration.
  DELAY_NS_100;
  digitalWrite(sclkPin, HIGH);
  DELAY_NS_100;
  digitalWrite(sclkPin, LOW);
  // DOUT goes low again once the calibration finishes.
  if (!safeWait()) {
    LOG_PRINTLN("ADC calibration error");
    return;
  }
  readADCWithWait();  // Discard the first conversion after calibration.
}

bool ADS1232::tare() {
  bool isStable = false;
  tareRaw = getRaw(isStable);
  return isStable;
}

void ADS1232::setCalFactor(double cal) { calFactor = cal; }

double ADS1232::getUnits() {
  bool isStable = false;
  // Multiplied in double: in float it would discard calFactor's extra precision.
  double raw = getRaw(isStable) - tareRaw;
  double units = raw * calFactor;

  // The first reading is always accepted.
  if (isnan(_lastUnitsFiltered)) {
    _lastUnitsFiltered = units;
    return units;
  }

  if (!isStable) {
    // While the buffer is unsettled, a jump this large is a glitch: keep the previous reading.
    constexpr float MAX_DELTA = 200.0f;  // grams
    float delta = units - _lastUnitsFiltered;
    if (delta > MAX_DELTA || delta < -MAX_DELTA) {
      return _lastUnitsFiltered;
    }
  }

  _lastUnitsFiltered = units;
  return units;
}

int32_t ADS1232::getRaw(bool &isStable) {
  int32_t rawSum = 0;
  for (uint8_t i = 0; i < ringBufferSize; ++i) {
    rawSum += ringBuffer[(ringBufferIndex + i) % ringBufferSize];
  }

  int32_t rawMean = rawSum / ringBufferSize;

  // If any sample deviates too far from the mean, only the latest measurement is returned.
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

/// Keeps the 24-bit shift-in uninterrupted.
static portMUX_TYPE s_adcMux = portMUX_INITIALIZER_UNLOCKED;

void ADS1232::readADC() {
  int32_t adcValue = 0;

  portENTER_CRITICAL(&s_adcMux);
  for (int i = 0; i < 24; i++) {
    digitalWrite(sclkPin, HIGH);
    adcValue = (adcValue << 1) + digitalRead(doutPin);
    digitalWrite(sclkPin, LOW);
  }
  digitalWrite(sclkPin, HIGH);  // The 25th pulse returns DOUT high.
  digitalWrite(sclkPin, LOW);
  portEXIT_CRITICAL(&s_adcMux);

  // Sign-extends the 24-bit two's-complement value.
  adcValue = (adcValue << 8) / 256;

  ringBufferIndex = (ringBufferIndex + 1) % ringBufferSize;
  ringBuffer[ringBufferIndex] = adcValue;
}
