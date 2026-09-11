#pragma once

// Task #1 -- Scale Sampling. Per rtos-architecture.md §3.1/§6.1: polls the
// ADS1232 driver's already-non-blocking readADCIfReady() on a short fixed
// period and pushes every new conversion onto g_scale_sample_q. Sole owner
// of the ADS1232 instance and its calibration/ring-buffer config (§2) --
// nothing else in the firmware touches the ADC directly.

void createScaleTask();
