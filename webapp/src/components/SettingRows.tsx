import { useEffect, useRef, useState, type ReactNode } from "react";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type {
  ActionSettingField,
  BooleanSettingField,
  NumericSettingField,
  WritableSettingsField,
} from "../lib/types";

/** Returns a function that writes `field` on the device. */
function useSettingWriter(field: WritableSettingsField) {
  const { send, nextRequestId } = useDeviceSocket();
  return (value: number | boolean) =>
    send({ type: "settings_write", field, value, request_id: nextRequestId() });
}

/** The device's current value of a numeric setting, or null until it has reported. */
function useNumericSetting(field: NumericSettingField): number | null {
  const { settings } = useDeviceSocket();
  return settings ? settings[field] : null;
}

function formatCurrent(value: number | null, unit?: string): string {
  return value !== null ? `${value}${unit ?? ""}` : "not reported yet";
}

/** The label, current value and control layout shared by every setting. */
function SettingRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="setting-row">
      <div className="label">{label}</div>
      <div style={{ display: "flex", alignItems: "center" }}>{children}</div>
    </div>
  );
}

interface NumberRowProps {
  field: NumericSettingField;
  label: string;
  step: string;
  unit?: string;
  /** Widens the input so a long value (the calibration factor) isn't clipped while typing. */
  wide?: boolean;
}

/** A typed number with a Set button. */
export function NumberSettingRow({ field, label, step, unit, wide }: NumberRowProps) {
  const current = useNumericSetting(field);
  const write = useSettingWriter(field);
  const [draft, setDraft] = useState("");

  function submit() {
    const value = parseFloat(draft);
    if (!Number.isFinite(value)) return;
    write(value);
    setDraft("");
  }

  return (
    <SettingRow label={label}>
      <span className="current">{formatCurrent(current, unit)}</span>
      <input
        type="number"
        step={step}
        style={wide ? { width: "12em" } : undefined}
        placeholder={current !== null ? String(current) : "new value"}
        value={draft}
        onChange={(e) => setDraft(e.target.value)}
        onKeyDown={(e) => e.key === "Enter" && submit()}
      />
      <button style={{ marginLeft: "0.5em" }} onClick={submit} disabled={draft === ""}>
        Set
      </button>
    </SettingRow>
  );
}

interface SelectRowProps {
  field: NumericSettingField;
  label: string;
  options: number[];
  unit?: string;
}

/** A choice among hardware-fixed values, where any other number would be rejected. */
export function SelectSettingRow({ field, label, options, unit }: SelectRowProps) {
  const current = useNumericSetting(field);
  const write = useSettingWriter(field);

  return (
    <SettingRow label={label}>
      <span className="current">{formatCurrent(current, unit)}</span>
      <select
        value={current ?? ""}
        onChange={(e) => {
          const value = parseInt(e.target.value, 10);
          if (Number.isFinite(value)) write(value);
        }}
      >
        {current === null && <option value="">--</option>}
        {options.map((opt) => (
          <option key={opt} value={opt}>
            {opt}
            {unit ?? ""}
          </option>
        ))}
      </select>
    </SettingRow>
  );
}

/** An on/off switch; the firmware only accepts a JSON boolean for these fields. */
export function ToggleSettingRow({ field, label }: { field: BooleanSettingField; label: string }) {
  const { settings } = useDeviceSocket();
  const write = useSettingWriter(field);
  const current = settings ? settings[field] : null;

  return (
    <SettingRow label={label}>
      <span className="current">{current === null ? "not reported yet" : current ? "on" : "off"}</span>
      <input
        type="checkbox"
        checked={current ?? false}
        disabled={current === null}
        onChange={(e) => write(e.target.checked)}
      />
    </SettingRow>
  );
}

interface SliderRowProps {
  field: NumericSettingField;
  label: string;
  min: number;
  max: number;
  step: number;
  unit?: string;
}

/**
 * A value dialed in by eye against the live device. Dragging updates the
 * number at once, but the write is debounced so a drag doesn't flood the
 * device's write queue or cause an NVS write per tick.
 */
export function SliderSettingRow({ field, label, min, max, step, unit }: SliderRowProps) {
  const current = useNumericSetting(field);
  const write = useSettingWriter(field);
  const [draft, setDraft] = useState<number | null>(null);
  const debounceRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  useEffect(() => () => clearTimeout(debounceRef.current), []);
  // Once the device's broadcast catches up, it takes over from the local draft again.
  useEffect(() => setDraft(null), [current]);

  function onDrag(value: number) {
    setDraft(value);
    clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(() => write(value), 200);
  }

  const shown = draft ?? current;

  return (
    <div className="setting-row">
      <div className="label">{label}</div>
      <div style={{ display: "flex", alignItems: "center", gap: "0.5em" }}>
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={shown ?? min}
          disabled={current === null}
          onChange={(e) => onDrag(parseFloat(e.target.value))}
          style={{ flex: 1 }}
        />
        <span className="current" style={{ minWidth: "3.5em", textAlign: "right" }}>
          {formatCurrent(shown, unit)}
        </span>
      </div>
    </div>
  );
}

/** A button that must be confirmed before it sends a one-shot action flag. */
export function ArmedActionButton({ label, field }: { label: string; field: ActionSettingField }) {
  const write = useSettingWriter(field);
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
          write(true);
          setArmed(false);
        }}
      >
        Confirm
      </button>
      <button onClick={() => setArmed(false)}>Cancel</button>
    </span>
  );
}
