# ADS1232 library

A fork of [jousis' ADS1232 library](https://gitlab.com/jousis/ads1232-library). But almost everything changed.

Main features

* Read an ADS1232 on an ESP32.
  * Fastest possible bit shift implementation without delays.
  * If you experience problems, just comment in the 100 ns delays.
  * In reality, my ADS1232 can keep up with the 240 MHz clock speed.
* Raw and calibrated measurements, tare.
  * Like many scale libraries, you can set a calibration factor and tare the ADC to calculate calibrated units, like grams.
* Non-blocking API.
  * Call `readADCIfReady()` in your `loop()`. This will read the ADC if a new value is available, but it won't wait for a value to become available. Other implementations that wait for data to be ready result in applications that spend most time waiting for the ADC. That is, a lot of wasted cycles.
  * Call `getRaw()` and `getUnits()` to get the raw ADC counts and calibrated grams, respectively. The values are collected from an internal ring buffer. Its size can be configured. I've achieved great results with a `10` SPS ADC speed and a ring buffer size of `12`.
* Responsiveness.
  * If huge variation in the raw ADC counts are observed, only the last measurement is returned. This results in a responsive readout, while still achieving high accuracy on the final weight.