// Display formatting only. Values are produced by the C engine; null means "undefined" (e.g. zero denominator).
import type { ProtocolId } from '../engine/types';

export const PROTOCOLS: ProtocolId[] = ['stop_and_wait', 'go_back_n', 'selective_repeat'];
export const PROTOCOL_LABEL: Record<ProtocolId, string> = { stop_and_wait: 'Stop-and-Wait', go_back_n: 'Go-Back-N', selective_repeat: 'Selective Repeat' };
export const PROTOCOL_SHORT: Record<ProtocolId, string> = { stop_and_wait: 'SW', go_back_n: 'GBN', selective_repeat: 'SR' };
// Categorical slots 1-3 of the validated palette (all-pairs CVD-safe on the dark surface); fixed per protocol.
export const PROTOCOL_COLOR: Record<ProtocolId, string> = { stop_and_wait: '#3987e5', go_back_n: '#d95926', selective_repeat: '#199e70' };

export const NA = 'n/a';

export function num(value: number | null | undefined, digits = 2): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return NA;
  return value.toLocaleString(undefined, { maximumFractionDigits: digits, minimumFractionDigits: 0 });
}

export function pct(value: number | null | undefined, digits = 1): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return NA;
  return `${(value * 100).toFixed(digits)}%`;
}

export function bps(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return NA;
  if (value >= 1e6) return `${(value / 1e6).toFixed(3)} Mbit/s`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)} kbit/s`;
  return `${value.toFixed(0)} bit/s`;
}

export function ms(value: number | null | undefined, digits = 1): string {
  return value === null || value === undefined || !Number.isFinite(value) ? NA : `${value.toFixed(digits)} ms`;
}

export function secs(value: number | null | undefined, digits = 3): string {
  return value === null || value === undefined || !Number.isFinite(value) ? NA : `${value.toFixed(digits)} s`;
}

export function titleCase(id: string): string {
  return id.replace(/_/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase());
}

export function sweepLabel(parameter: string, value: number | null): string {
  if (value === null) return 'single run';
  if (parameter === 'congestion_mode') return ['none', 'rate AIMD', 'window AIMD'][value] ?? String(value);
  if (parameter.endsWith('probability')) return `${(value * 100).toFixed(0)}%`;
  if (parameter === 'timeout_s') return `${(value * 1000).toFixed(0)} ms`;
  return String(value);
}
