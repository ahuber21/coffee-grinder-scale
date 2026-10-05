import { useState } from "react";
import {
  ArmedActionButton,
  NumberSettingRow,
  SelectSettingRow,
  SliderSettingRow,
  ToggleSettingRow,
} from "../components/SettingRows";
import { useDeviceSocket } from "../lib/DeviceSocketContext";

const noteStyle = { fontSize: "0.85em" };

export default function SettingsPage() {
  const { lastError } = useDeviceSocket();
  const [showAdvanced, setShowAdvanced] = useState(false);

  return (
    <>
      <div className="panel">
        <h3>Dosing</h3>
        <NumberSettingRow field="target_dose_single" label="Target dose (single)" step="0.1" unit=" g" />
        <NumberSettingRow field="target_dose_double" label="Target dose (double)" step="0.1" unit=" g" />
        <NumberSettingRow field="top_up_margin_single" label="Top-up margin (single)" step="0.01" unit=" g" />
        <NumberSettingRow field="top_up_margin_double" label="Top-up margin (double)" step="0.01" unit=" g" />
      </div>

      <div className="panel">
        <h3>Scale</h3>
        <NumberSettingRow
          field="calibration_factor_x1e6"
          label="Calibration factor (×10⁻⁶)"
          step="0.000001"
          wide
        />
        <p className="muted" style={noteStyle}>
          Shown and set scaled by 1e6 (e.g. 924.583895, not 0.000924583895): JSON over the wire
          carries only 9 decimal places, which would truncate the true factor. Check it against
          a known reference weight before setting it.
        </p>
      </div>

      <div className="panel">
        <h3>Input</h3>
        <NumberSettingRow field="button_debounce_ms" label="Button debounce" step="10" unit=" ms" />
      </div>

      <div className="panel">
        <h3>Landing learner</h3>
        <ToggleSettingRow field="landing_learner_enabled" label="Learner active" />
        <NumberSettingRow field="landing_learner_rate" label="Learning rate (per session)" step="0.01" />
        <NumberSettingRow field="landing_learner_clamp_ms" label="Stop-time clamp (±)" step="50" unit=" ms" />
        <p className="muted" style={noteStyle}>
          Shifts the main-grind stop so the settled weight lands on the margin-offset target
          (requested dose minus the top-up margin). The clamp bounds how far it can move the
          stop from the unlearned stop time; the rate is the per-session forgetting (0.05 weighs
          about the last 20 sessions). Each session's landing and applied shift show up in the
          console as a LAND line.
        </p>
      </div>

      <div className="panel">
        <h3>Display animation</h3>
        <SliderSettingRow field="display_clump_density" label="Pixel density" min={0} max={3} step={0.1} />
        <SliderSettingRow field="display_clump_gravity" label="Gravity" min={0.1} max={5} step={0.1} />
        <p className="muted" style={noteStyle}>
          Tunes the falling-grounds animation on the GRINDING and firmware-update screens:
          density scales how many pieces fall at once (0 turns the animation off), gravity
          scales how fast they fall. Each change reaches the screen within about a frame.
        </p>
      </div>

      <div className="panel">
        <h3>WiFi</h3>
        <div style={{ display: "flex", gap: "1rem", flexWrap: "wrap" }}>
          <ArmedActionButton label="Reset WiFi credentials" field="wifi_reset_flag" />
          <ArmedActionButton label="Reboot device" field="wifi_reboot_flag" />
        </div>
      </div>

      <button onClick={() => setShowAdvanced((v) => !v)}>
        {showAdvanced ? "Hide advanced settings" : "Show advanced settings"}
      </button>

      {showAdvanced && (
        <>
          <div className="panel">
            <h3>ADC (advanced)</h3>
            <SelectSettingRow field="gain" label="Gain" options={[1, 2, 64, 128]} />
            <SelectSettingRow field="speed" label="Speed" options={[10, 80]} unit=" SPS" />
            <NumberSettingRow field="read_samples" label="Ring buffer window" step="1" unit=" samples" />
          </div>

          <div className="panel">
            <h3>Top-up (advanced)</h3>
            <NumberSettingRow field="min_topup_grams" label="Smallest gap to top up" step="0.01" unit=" g" />
          </div>

          <div className="panel">
            <h3>Timeouts (advanced)</h3>
            <NumberSettingRow field="grinding_timeout_ms" label="Grinding safety timeout" step="100" unit=" ms" />
            <NumberSettingRow field="topup_timeout_ms" label="Top-up timeout" step="100" unit=" ms" />
            <NumberSettingRow field="finalize_timeout_ms" label="Finalize screen duration" step="100" unit=" ms" />
            <NumberSettingRow field="confirm_timeout_ms" label="Confirm screen timeout" step="100" unit=" ms" />
            <NumberSettingRow field="stability_min_wait_ms" label="Stability min wait" step="10" unit=" ms" />
            <NumberSettingRow field="stability_max_wait_ms" label="Stability max wait" step="10" unit=" ms" />
            <NumberSettingRow field="screensaver_timeout_s" label="Screensaver idle timeout" step="1" unit=" s" />
            <NumberSettingRow
              field="screensaver_wake_weight_delta_g"
              label="Screensaver wake weight delta"
              step="0.1"
              unit=" g"
            />
            <NumberSettingRow field="button_min_hold_ms" label="Button min hold" step="1" unit=" ms" />
          </div>
        </>
      )}

      {lastError && (
        <div className="panel" style={{ borderColor: "var(--red)" }}>
          <span style={{ color: "var(--red)" }}>Last error:</span> {lastError.message}
        </div>
      )}
    </>
  );
}
