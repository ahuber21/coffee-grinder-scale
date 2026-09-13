import { useState } from "react";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { WritableSettingsField } from "../lib/types";

interface NumberFieldProps {
  field: WritableSettingsField;
  label: string;
  currentValue: number | null;
  step: string;
  unit?: string;
}

function NumberSettingRow({ field, label, currentValue, step, unit }: NumberFieldProps) {
  const { send, nextRequestId } = useDeviceSocket();
  const [draft, setDraft] = useState("");

  function submit() {
    const value = parseFloat(draft);
    if (!Number.isFinite(value)) return;
    send({ type: "settings_write", field, value, request_id: nextRequestId() });
    setDraft("");
  }

  return (
    <div className="setting-row">
      <div className="label">{label}</div>
      <div style={{ display: "flex", alignItems: "center" }}>
        <span className="current">
          {currentValue !== null ? `${currentValue}${unit ?? ""}` : "not reported yet"}
        </span>
        <input
          type="number"
          step={step}
          placeholder={currentValue !== null ? String(currentValue) : "new value"}
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && submit()}
        />
        <button style={{ marginLeft: "0.5em" }} onClick={submit} disabled={draft === ""}>
          Set
        </button>
      </div>
    </div>
  );
}

interface SelectFieldProps {
  field: WritableSettingsField;
  label: string;
  currentValue: number | null;
  options: number[];
  unit?: string;
}

// For the ADS1232 driver's hardware-fixed choices (gain, speed) where any
// other value is meaningless -- a free-typed number invites a rejected
// write. Exported: the Advanced/Calibration page reuses this for its own
// live SPS toggle rather than duplicating the write-request plumbing.
export function SelectSettingRow({ field, label, currentValue, options, unit }: SelectFieldProps) {
  const { send, nextRequestId } = useDeviceSocket();

  return (
    <div className="setting-row">
      <div className="label">{label}</div>
      <div style={{ display: "flex", alignItems: "center" }}>
        <span className="current">
          {currentValue !== null ? `${currentValue}${unit ?? ""}` : "not reported yet"}
        </span>
        <select
          value={currentValue ?? ""}
          onChange={(e) => {
            const value = parseInt(e.target.value, 10);
            if (!Number.isFinite(value)) return;
            send({ type: "settings_write", field, value, request_id: nextRequestId() });
          }}
        >
          {currentValue === null && <option value="">--</option>}
          {options.map((opt) => (
            <option key={opt} value={opt}>
              {opt}
              {unit ?? ""}
            </option>
          ))}
        </select>
      </div>
    </div>
  );
}

function ArmedActionButton({
  label,
  field,
}: {
  label: string;
  field: "wifi_reset_flag" | "wifi_reboot_flag";
}) {
  const { send, nextRequestId } = useDeviceSocket();
  const [armed, setArmed] = useState(false);

  if (!armed) {
    return (
      <button className="danger" onClick={() => setArmed(true)}>
        {label}
      </button>
    );
  }
  return (
    <span style={{ display: "inline-flex", gap: "0.5em", alignItems: "center" }}>
      <span className="muted">Reboots the device -- confirm?</span>
      <button
        className="danger"
        onClick={() => {
          send({ type: "settings_write", field, value: true, request_id: nextRequestId() });
          setArmed(false);
        }}
      >
        Confirm
      </button>
      <button onClick={() => setArmed(false)}>Cancel</button>
    </span>
  );
}

export default function SettingsPage() {
  const { settings, lastError } = useDeviceSocket();
  const [showAdvanced, setShowAdvanced] = useState(false);

  return (
    <>
      <div className="panel">
        <h3>Dosing</h3>
        <NumberSettingRow
          field="target_dose_single"
          label="Target dose (single)"
          currentValue={settings?.target_dose_single ?? null}
          step="0.1"
          unit=" g"
        />
        <NumberSettingRow
          field="target_dose_double"
          label="Target dose (double)"
          currentValue={settings?.target_dose_double ?? null}
          step="0.1"
          unit=" g"
        />
        <NumberSettingRow
          field="top_up_margin_single"
          label="Top-up margin (single)"
          currentValue={settings?.top_up_margin_single ?? null}
          step="0.01"
          unit=" g"
        />
        <NumberSettingRow
          field="top_up_margin_double"
          label="Top-up margin (double)"
          currentValue={settings?.top_up_margin_double ?? null}
          step="0.01"
          unit=" g"
        />
      </div>

      <div className="panel">
        <h3>Scale</h3>
        <NumberSettingRow
          field="calibration_factor_x1e6"
          label="Calibration factor (×10⁻⁶)"
          currentValue={settings?.calibration_factor_x1e6 ?? null}
          step="0.000001"
        />
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Shown and set scaled by 1e6 (e.g. 924.583895, not 0.000924583895) -- the true factor
          has more significant digits than fit in 9 decimal places once its actual magnitude
          (~1e-4) is accounted for, and JSON over the wire only carries 9. Double-check against
          a known reference weight before setting it.
        </p>
      </div>

      <div className="panel">
        <h3>Input</h3>
        <NumberSettingRow
          field="button_debounce_ms"
          label="Button debounce"
          currentValue={settings?.button_debounce_ms ?? null}
          step="10"
          unit=" ms"
        />
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
            <SelectSettingRow
              field="gain"
              label="Gain"
              currentValue={settings?.gain ?? null}
              options={[1, 2, 64, 128]}
            />
            <SelectSettingRow
              field="speed"
              label="Speed"
              currentValue={settings?.speed ?? null}
              options={[10, 80]}
              unit=" SPS"
            />
            <NumberSettingRow
              field="read_samples"
              label="Ring buffer window"
              currentValue={settings?.read_samples ?? null}
              step="1"
              unit=" samples"
            />
          </div>

          <div className="panel">
            <h3>Topup model (advanced)</h3>
            <NumberSettingRow
              field="min_topup_grams"
              label="Min top-up pulse"
              currentValue={settings?.min_topup_grams ?? null}
              step="0.01"
              unit=" g"
            />
            <NumberSettingRow
              field="rate_calculation_percentage"
              label="Rate calculation fraction"
              currentValue={settings?.rate_calculation_percentage ?? null}
              step="0.01"
            />
          </div>

          <div className="panel">
            <h3>Timeouts (advanced)</h3>
            <NumberSettingRow
              field="grinding_timeout_ms"
              label="Grinding safety timeout"
              currentValue={settings?.grinding_timeout_ms ?? null}
              step="100"
              unit=" ms"
            />
            <NumberSettingRow
              field="topup_timeout_ms"
              label="Top-up timeout"
              currentValue={settings?.topup_timeout_ms ?? null}
              step="100"
              unit=" ms"
            />
            <NumberSettingRow
              field="finalize_timeout_ms"
              label="Finalize screen duration"
              currentValue={settings?.finalize_timeout_ms ?? null}
              step="100"
              unit=" ms"
            />
            <NumberSettingRow
              field="confirm_timeout_ms"
              label="Confirm screen timeout"
              currentValue={settings?.confirm_timeout_ms ?? null}
              step="100"
              unit=" ms"
            />
            <NumberSettingRow
              field="stability_min_wait_ms"
              label="Stability min wait"
              currentValue={settings?.stability_min_wait_ms ?? null}
              step="10"
              unit=" ms"
            />
            <NumberSettingRow
              field="stability_max_wait_ms"
              label="Stability max wait"
              currentValue={settings?.stability_max_wait_ms ?? null}
              step="10"
              unit=" ms"
            />
            <NumberSettingRow
              field="min_topup_runtime_ms"
              label="Min top-up pulse runtime"
              currentValue={settings?.min_topup_runtime_ms ?? null}
              step="10"
              unit=" ms"
            />
            <NumberSettingRow
              field="min_topup_interval_ms"
              label="Min top-up pulse interval"
              currentValue={settings?.min_topup_interval_ms ?? null}
              step="10"
              unit=" ms"
            />
            <NumberSettingRow
              field="screensaver_timeout_s"
              label="Screensaver idle timeout"
              currentValue={settings?.screensaver_timeout_s ?? null}
              step="1"
              unit=" s"
            />
            <NumberSettingRow
              field="screensaver_wake_weight_delta_g"
              label="Screensaver wake weight delta"
              currentValue={settings?.screensaver_wake_weight_delta_g ?? null}
              step="0.1"
              unit=" g"
            />
            <NumberSettingRow
              field="button_min_hold_ms"
              label="Button min hold"
              currentValue={settings?.button_min_hold_ms ?? null}
              step="1"
              unit=" ms"
            />
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
