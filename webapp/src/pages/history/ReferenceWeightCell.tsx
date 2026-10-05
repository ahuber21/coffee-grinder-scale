import { useEffect, useRef, useState } from "react";
import { patchReferenceWeight, type Session } from "../../lib/postgrest";

interface Props {
  session: Session;
  onSaved: (value: number | null) => void;
}

/**
 * Click-to-edit reading from an independent reference scale. It is entered
 * by hand after weighing the finished dose, so it can't arrive with the
 * session row and has to be editable afterwards.
 */
export default function ReferenceWeightCell({ session, onSaved }: Props) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState("");
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState(false);
  const inputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    if (editing) inputRef.current?.focus();
  }, [editing]);

  function startEditing() {
    setDraft(session.reference_weight_g?.toString() ?? "");
    setError(false);
    setEditing(true);
  }

  async function save() {
    const trimmed = draft.trim();
    const value = trimmed === "" ? null : Number(trimmed);
    if (value !== null && (!Number.isFinite(value) || value <= 0)) {
      setError(true);
      return;
    }
    setSaving(true);
    try {
      await patchReferenceWeight(session.session_id, value);
      onSaved(value);
      setEditing(false);
    } catch {
      setError(true);
    } finally {
      setSaving(false);
    }
  }

  if (editing) {
    return (
      <span style={{ display: "inline-flex", alignItems: "center", gap: "0.4em" }}>
        <input
          ref={inputRef}
          type="number"
          step="0.01"
          inputMode="decimal"
          value={draft}
          disabled={saving}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") save();
            if (e.key === "Escape") setEditing(false);
          }}
          onBlur={save}
          onClick={(e) => e.stopPropagation()}
          style={{ width: "5.5em", padding: "0.2em 0.4em" }}
        />
        {error && <span style={{ color: "var(--red)", fontSize: "0.85em" }}>invalid</span>}
      </span>
    );
  }

  const delta =
    session.reference_weight_g !== null && session.final_weight_g !== null
      ? session.reference_weight_g - session.final_weight_g
      : null;

  return (
    <span
      className="ref-weight-cell"
      onClick={(e) => {
        e.stopPropagation();
        startEditing();
      }}
      title="Click to edit"
      style={{ cursor: "pointer" }}
    >
      {session.reference_weight_g !== null ? (
        <>
          {session.reference_weight_g.toFixed(2)}g
          {delta !== null && (
            <span className="muted" style={{ fontSize: "0.85em" }}>
              {" "}
              (Δ{delta >= 0 ? "+" : ""}
              {delta.toFixed(2)})
            </span>
          )}
        </>
      ) : (
        <span className="muted">+ add</span>
      )}
    </span>
  );
}
