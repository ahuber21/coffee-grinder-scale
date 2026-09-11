#pragma once

/**
 * Scale Sampling task. Polls the ADS1232 driver's non-blocking
 * readADCIfReady() on a short fixed period and pushes every new
 * conversion onto a queue for Dosing task. Sole owner of the ADS1232
 * instance and its calibration/ring-buffer config -- nothing else in
 * the firmware touches the ADC directly.
 */

/** Creates and starts the Scale task. */
void createScaleTask();
