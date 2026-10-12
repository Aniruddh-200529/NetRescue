import type { ReactNode } from 'react';

interface NumberFieldProps {
  label: string; value: number; onChange: (value: number) => void; min?: number; max?: number; step?: number;
  hint?: ReactNode; disabled?: boolean; suffix?: string;
}

/** Numeric input that only reports finite values inside [min, max]; the engine validates again. */
export function NumberField({ label, value, onChange, min, max, step = 1, hint, disabled, suffix }: NumberFieldProps) {
  return (
    <div className="field">
      <label>{label}{suffix && <span className="muted">{suffix}</span>}</label>
      <input
        type="number" value={Number.isFinite(value) ? value : ''} min={min} max={max} step={step} disabled={disabled}
        aria-label={label}
        onChange={(e) => {
          const v = e.target.valueAsNumber;
          if (!Number.isFinite(v)) return;
          if (min !== undefined && v < min) return;
          if (max !== undefined && v > max) return;
          onChange(v);
        }}
      />
      {hint && <span className="hint">{hint}</span>}
    </div>
  );
}

interface ProbabilityFieldProps { label: string; value: number; onChange: (value: number) => void; disabled?: boolean; hint?: ReactNode; max?: number }

/** Probability slider shown as a percentage (0-100%). */
export function ProbabilityField({ label, value, onChange, disabled, hint, max = 1 }: ProbabilityFieldProps) {
  return (
    <div className="field">
      <label>{label}<b className="mono">{(value * 100).toFixed(0)}%</b></label>
      <input type="range" min={0} max={max} step={0.01} value={value} disabled={disabled} aria-label={label}
        onChange={(e) => onChange(Number(e.target.value))} />
      {hint && <span className="hint">{hint}</span>}
    </div>
  );
}

export function SelectField<T extends string>({ label, value, options, onChange, disabled }: {
  label: string; value: T; options: { value: T; label: string }[]; onChange: (value: T) => void; disabled?: boolean;
}) {
  return (
    <div className="field">
      <label>{label}</label>
      <select value={value} disabled={disabled} aria-label={label} onChange={(e) => onChange(e.target.value as T)}>
        {options.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
      </select>
    </div>
  );
}

export function Panel({ title, actions, children, className = '' }: { title: ReactNode; actions?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`panel ${className}`}>
      <div className="panel-head"><h3>{title}</h3>{actions}</div>
      <div className="panel-body">{children}</div>
    </section>
  );
}
