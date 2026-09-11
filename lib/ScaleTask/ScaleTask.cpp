#include "ScaleTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ADS1232.h"
#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"

namespace {

ADS1232 g_ads(ADC_PDWN_PIN, ADC_SCLK_PIN, ADC_DOUT_PIN, ADC_SPEED_PIN,
              ADC_GAIN1_PIN, ADC_GAIN0_PIN);

// Re-tares automatically whenever idle and stable, rate-limited only to
// avoid calling tare() on every single sample.
constexpr uint32_t kAutoTareMinIntervalMs = 1000;
uint32_t g_last_auto_tare_ms = 0;

/** Version of SettingsSnapshot last applied to the ADS1232 driver. */
uint32_t g_applied_settings_version = 0;

/** Re-applies ADC config to the ADS1232 driver if a new settings version arrived. */
void applySettingsIfChanged() {
  SettingsSnapshot snap;
  // Peek, never consume: Scale task is the only reader of this mailbox,
  // and peeking never empties it.
  if (xQueuePeek(g_settings_mailbox_scale, &snap, 0) != pdTRUE) {
    return;
  }
  if (snap.version == g_applied_settings_version) {
    return;
  }
  g_ads.setRingBufferSize(snap.read_samples);
  g_ads.setSpeed(snap.speed);
  g_ads.setGain(snap.gain);
  g_ads.setCalFactor(snap.calibration_factor);
  g_applied_settings_version = snap.version;
}

/** Scale task entry point: initializes the ADS1232, then samples at ~500Hz. */
void scaleTaskFn(void *) {
  // Wait for Settings task's first snapshot before touching the ADC, so
  // calibration is never applied from a default-constructed placeholder.
  xEventGroupWaitBits(g_sys_events, kSettingsLoadedBit, pdFALSE, pdTRUE,
                       portMAX_DELAY);

  // Powers the load cell's bridge excitation -- without it the ADC
  // reads a fixed value regardless of physical force.
  pinMode(ADC_LDO_EN_PIN, OUTPUT);
  digitalWrite(ADC_LDO_EN_PIN, HIGH);

  // begin() (which also powers on and calibrates) resets gain/speed to
  // hardware defaults, so real settings must be applied after it, not
  // before -- and before initRingBuffer(), which reads ringBufferSize.
  g_ads.begin();
  applySettingsIfChanged();
  g_ads.initRingBuffer();
  g_ads.tare();

  xEventGroupSetBits(g_sys_events, kScaleReadyBit);

  uint32_t seq = 0;
  const TickType_t period = pdMS_TO_TICKS(2);  // ~500Hz poll.
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    applySettingsIfChanged();

    g_ads.readADCIfReady();

    bool stable = false;
    int32_t raw = g_ads.getRaw(stable);

    ScaleSample sample{
        .grams = static_cast<float>(g_ads.getUnits()),
        .raw_adc = raw,
        .stable = stable,
        .sample_seq = seq++,
        .millis = millis(),
    };

    bool dosing_active = (xEventGroupGetBits(g_sys_events) & kDosingActiveBit) != 0;
    if (!dosing_active && stable &&
        sample.millis - g_last_auto_tare_ms >= kAutoTareMinIntervalMs) {
      g_ads.tare();
      g_last_auto_tare_ms = sample.millis;
    }

    // Zero-timeout send; on a full queue (Dosing task stalled), drop the
    // oldest sample rather than block the sampler.
    if (xQueueSend(g_scale_sample_q, &sample, 0) != pdTRUE) {
      ScaleSample discard;
      xQueueReceive(g_scale_sample_q, &discard, 0);
      xQueueSend(g_scale_sample_q, &sample, 0);
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

}  // namespace

void createScaleTask() {
  xTaskCreatePinnedToCore(scaleTaskFn, "Scale", TaskConfig::kScaleStackBytes,
                           nullptr, TaskConfig::kScalePriority, nullptr,
                           TaskConfig::kScaleCore);
}
