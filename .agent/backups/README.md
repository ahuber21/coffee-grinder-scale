# Device state backups

`GET /api/boot_backup` returns the settings and the raw NVS model blobs (`topup_model` =
`DosingModelState`, `land_learner` = `LandingLearnerState`, hex, little-endian) as they were
when the device last booted. Reboot the device, then fetch it, to snapshot the current state.

## device-state-2026-10-22-pre-margin0.json

State before the switch to margin 0 (aiming the main grind at the real target).
Both margins were 0.5, `min_topup_grams` 0.08, learner enabled/0.05/500 ms.

- Restoring the topup behaviour: set `top_up_margin_single` and `top_up_margin_double` back to 0.5.
- The blobs were written by the firmware before the rate-precision fix: `rate_precision` is
  saturated (2.7e9) and `rate_n_effective` is 1.56M. The current firmware clamps this on load.
- Decoded: rate_hat 0.9615 g/s, coast 0.326 g, topup LUT durations (ms) 434 443 431 662 759
  749 869 849 1062 1264 with counts 17 111 72 52 66 45 21 10 5 11; learner sessions
  34 single / 53 double.
- Restoring the blobs themselves needs a small import path (none exists yet); the hex is the
  exact bytes to write back under the same NVS keys.
