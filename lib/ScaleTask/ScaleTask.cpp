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

ADS1232 g_ads(ADC_PDWN_PIN, ADC_SCLK_PIN, ADC_DOUT_PIN, ADC_SPEED_PIN, ADC_GAIN1_PIN, ADC_GAIN0_PIN);

/// While idle and stable the scale re-tares itself, at most this often.
constexpr uint32_t kAutoTareMinIntervalMs = 1000;
/// After a dose finishes, auto-tare waits this long so the final weight stays on screen.
constexpr uint32_t kAutoTareIdleReturnCooldownMs = 10000;
constexpr uint32_t kBootTareTimeoutMs = 5000;
constexpr uint32_t kPreDoseTareTimeoutMs = 20000;
constexpr uint32_t kTareWarnIntervalMs = 2000;
constexpr uint32_t kSamplePeriodMs = 2;  ///< ~500Hz.

uint32_t g_next_auto_tare_allowed_ms = 0;
bool g_prev_dosing_active = false;

/// Version of SettingsSnapshot last applied to the ADS1232 driver.
uint32_t g_applied_settings_version = 0;

/** Re-applies ADC config to the ADS1232 driver if a new settings version arrived. */
void applySettingsIfChanged() {
  SettingsSnapshot snap;
  // Peek, never consume: this task is the mailbox's only reader.
  if (xQueuePeek(g_settings_mailbox_scale, &snap, 0) != pdTRUE ||
      snap.version == g_applied_settings_version) {
    return;
  }
  g_ads.setRingBufferSize(snap.read_samples);
  g_ads.setSpeed(snap.speed);
  g_ads.setGain(snap.gain);
  g_ads.setCalFactor(snap.calibration_factor);
  g_applied_settings_version = snap.version;
}

/**
 * Tares once the ring buffer settles, retrying for up to `timeout_ms`
 * (tare() captures the buffer's mean whether or not it is stable, so it
 * reports when it did not). Returns whether a stable tare landed; the
 * bound only keeps a load cell that never settles from stalling this
 * task and everything downstream of its samples.
 */
bool tareUntilStable(uint32_t timeout_ms, const char *what) {
  uint32_t start_ms = millis();
  uint32_t last_warn_ms = start_ms;
  bool tared = g_ads.tare();
  while (!tared && millis() - start_ms < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(2));
    g_ads.readADCIfReady();
    tared = g_ads.tare();
    uint32_t now = millis();
    if (now - last_warn_ms >= kTareWarnIntervalMs) {
      Serial.printf("[Scale] %s tare still not stable after %u ms\n", what,
                    static_cast<unsigned>(now - start_ms));
      last_warn_ms = now;
    }
  }
  return tared;
}

/**
 * Answers Dosing task's per-dose re-tare requests before this tick's
 * sample is built, so the next sample already reflects the tare. On
 * timeout the tare is left as it was and failure is reported: an untared
 * dose is worse than a slow one, so Dosing aborts rather than start
 * from an unproven baseline.
 */
void handleTareRequests() {
  TareRequest request;
  while (xQueueReceive(g_tare_request_q, &request, 0) == pdTRUE) {
    bool tared = tareUntilStable(kPreDoseTareTimeoutMs, "pre-dose");
    if (!tared) {
      Serial.println("[Scale] pre-dose tare never stabilized -- aborting this dose");
    }
    TareResult result{tared};
    xQueueSend(g_tare_result_q, &result, 0);
  }
}

/** Tares an idle scale on its own, holding off right after a dose so the result stays readable. */
void autoTareWhenIdle(const ScaleSample &sample) {
  bool dosing_active = (xEventGroupGetBits(g_sys_events) & kDosingActiveBit) != 0;
  if (g_prev_dosing_active && !dosing_active) {
    g_next_auto_tare_allowed_ms = sample.millis + kAutoTareIdleReturnCooldownMs;
  }
  g_prev_dosing_active = dosing_active;

  if (!dosing_active && sample.stable && sample.millis >= g_next_auto_tare_allowed_ms) {
    g_ads.tare();
    g_next_auto_tare_allowed_ms = sample.millis + kAutoTareMinIntervalMs;
  }
}

/** Scale task entry point: initializes the ADS1232, then samples at ~500Hz. */
void scaleTaskFn(void *) {
  // Never apply calibration from a default-constructed placeholder.
  xEventGroupWaitBits(g_sys_events, kSettingsLoadedBit, pdFALSE, pdTRUE, portMAX_DELAY);

  // Powers the load cell's bridge excitation; without it the ADC reads a fixed value.
  pinMode(ADC_LDO_EN_PIN, OUTPUT);
  digitalWrite(ADC_LDO_EN_PIN, HIGH);

  // begin() resets gain/speed to hardware defaults, so settings go after it
  // and before initRingBuffer(), which reads the ring buffer size.
  g_ads.begin();
  applySettingsIfChanged();
  g_ads.initRingBuffer();

  // A mid-transient boot tare would bias every dose until the idle auto-tare corrects it.
  if (!tareUntilStable(kBootTareTimeoutMs, "boot")) {
    Serial.println("[Scale] boot tare never stabilized; using last reading");
  }

  xEventGroupSetBits(g_sys_events, kScaleReadyBit);

  uint32_t seq = 0;
  const TickType_t period = pdMS_TO_TICKS(kSamplePeriodMs);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    applySettingsIfChanged();
    g_ads.readADCIfReady();
    handleTareRequests();

    bool stable = false;
    int32_t raw = g_ads.getRaw(stable);
    ScaleSample sample{
        .grams = static_cast<float>(g_ads.getUnits()),
        .raw_adc = raw,
        .stable = stable,
        .sample_seq = seq++,
        .millis = millis(),
    };

    // The mailbox always holds the latest sample, with no backlog (see Queues.h).
    xQueueOverwrite(g_latest_sample_mailbox, &sample);
    autoTareWhenIdle(sample);

    // If Dosing task has stalled and the queue is full, drop the oldest sample rather than block.
    if (xQueueSend(g_scale_sample_q, &sample, 0) != pdTRUE) {
      ScaleSample discard;
      xQueueReceive(g_scale_sample_q, &discard, 0);
      xQueueSend(g_scale_sample_q, &sample, 0);
    }

    vTaskDelayUntil(&last_wake, period);
  }
}

}  // namespace

void createScaleTask() {
  xTaskCreatePinnedToCore(scaleTaskFn, "Scale", TaskConfig::kScaleStackBytes, nullptr,
                          TaskConfig::kScalePriority, nullptr, TaskConfig::kScaleCore);
}
