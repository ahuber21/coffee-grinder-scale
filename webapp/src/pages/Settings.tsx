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
          field="calibration_factor"
          label="Calibration factor"
          currentValue={settings?.calibration_factor ?? null}
          step="0.0001"
        />
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Double-check against a known reference weight before setting it.
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

      {lastError && (
        <div className="panel" style={{ borderColor: "var(--red)" }}>
          <span style={{ color: "var(--red)" }}>Last error:</span> {lastError.message}
        </div>
      )}
    </>
  );
}
