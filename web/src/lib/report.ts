// Builds a standalone HTML experiment report from an engine result (no values are recomputed).
import type { ExperimentResult, MetricDefinition, Stat } from '../engine/types';
import { bps, ms, num, pct, PROTOCOL_COLOR, PROTOCOL_LABEL, secs, sweepLabel, titleCase } from './format';

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!));

const METRICS: { key: string; label: string; format: (v: number | null) => string; higherBetter: boolean }[] = [
  { key: 'delivery_ratio', label: 'Packet delivery ratio', format: pct, higherBetter: true },
  { key: 'goodput_bps', label: 'Goodput', format: bps, higherBetter: true },
  { key: 'throughput_bps', label: 'Throughput (all bits on the wire)', format: bps, higherBetter: true },
  { key: 'latency_avg_ms', label: 'Average end-to-end latency', format: ms, higherBetter: false },
  { key: 'completion_time_s', label: 'Completion time', format: secs, higherBetter: false },
  { key: 'retransmissions', label: 'Data retransmissions', format: (v) => num(v, 1), higherBetter: false },
  { key: 'retransmission_rate', label: 'Retransmission rate', format: (v) => num(v, 3), higherBetter: false },
  { key: 'efficiency', label: 'Protocol efficiency', format: pct, higherBetter: true },
  { key: 'retx_unnecessary', label: 'Unnecessary retransmissions', format: (v) => num(v, 1), higherBetter: false },
  { key: 'faults_injected', label: 'Faults injected', format: (v) => num(v, 1), higherBetter: false },
  { key: 'faults_recovered', label: 'Faults recovered', format: (v) => num(v, 1), higherBetter: true },
  { key: 'queue_drops', label: 'Queue drops', format: (v) => num(v, 1), higherBetter: false },
  { key: 'queue_avg_occupancy', label: 'Bottleneck queue occupancy', format: (v) => num(v, 2), higherBetter: false },
];

function cell(s: Stat | undefined, format: (v: number | null) => string, trials: number): string {
  if (!s || s.n === 0) return 'n/a';
  if (trials <= 1) return format(s.mean);
  return `${format(s.mean)} <small>± ${format(s.stddev)}<br>95% CI ± ${format(s.ci95)} (n=${s.n})</small>`;
}

export function buildReport(result: ExperimentResult, definitions: MetricDefinition[]): string {
  const sweep = result.spec.sweep.parameter !== 'none';
  const c = result.spec.config;
  const metricSections = METRICS.map((m) => {
    const values = result.aggregates.map((a) => a.stats[m.key]?.mean ?? 0).filter((v) => Number.isFinite(v));
    const max = Math.max(1e-12, ...values.map(Math.abs));
    const rows = result.aggregates.map((a) => {
      const s = a.stats[m.key];
      const width = s?.mean != null ? Math.max(0.5, (Math.abs(s.mean) / max) * 100) : 0;
      return `<tr>${sweep ? `<td>${esc(sweepLabel(result.spec.sweep.parameter, a.sweep_value))}</td>` : ''}<td><span class="sw" style="background:${PROTOCOL_COLOR[a.protocol]}"></span>${PROTOCOL_LABEL[a.protocol]}</td>
        <td class="bar"><div style="width:${width.toFixed(1)}%;background:${PROTOCOL_COLOR[a.protocol]}"></div></td><td class="num">${cell(s, m.format, result.spec.trials)}</td></tr>`;
    }).join('');
    const def = definitions.find((d) => d.key === m.key);
    return `<section><h3>${m.label}</h3>${def ? `<p class="formula">${esc(def.formula)}</p>` : ''}
      <table><thead><tr>${sweep ? `<th>${esc(titleCase(result.spec.sweep.parameter))}</th>` : ''}<th>Protocol</th><th style="width:40%"></th><th class="num">${result.spec.trials > 1 ? 'mean ± std. dev.' : 'value'}</th></tr></thead><tbody>${rows}</tbody></table></section>`;
  }).join('');

  const runs = result.runs.map((r) => `<tr>${sweep ? `<td>${esc(sweepLabel(result.spec.sweep.parameter, r.sweep_value))}</td>` : ''}<td>${PROTOCOL_LABEL[r.protocol]}</td><td>${r.trial}</td><td>${r.seed}</td>
    <td>${r.metrics.completed ? 'yes' : esc(r.metrics.stop_reason)}</td><td class="num">${pct(r.metrics.delivery_ratio)}</td><td class="num">${bps(r.metrics.goodput_bps)}</td>
    <td class="num">${ms(r.metrics.latency_avg_ms)}</td><td class="num">${secs(r.metrics.elapsed_s)}</td><td class="num">${r.metrics.retransmissions}</td>
    <td>${r.retransmitted.slice(0, 40).map((x) => x.seq).join(', ')}${r.retransmitted_sequences > 40 ? ' …' : ''}</td></tr>`).join('');

  const rules = result.spec.scenario.length
    ? `<ul>${result.spec.scenario.map((r) => `<li>${esc(JSON.stringify(r))}</li>`).join('')}</ul>` : '<p>None (random impairments only).</p>';

  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>NetRescue experiment report</title>
<style>
body{font-family:Segoe UI,system-ui,sans-serif;margin:0;padding:24px;max-width:1100px;margin:auto;color:#1b2430;background:#fff;line-height:1.45}
h1{margin:0 0 4px}h2{margin-top:28px;border-bottom:2px solid #e3e8ef;padding-bottom:4px}h3{margin:18px 0 4px}
table{border-collapse:collapse;width:100%;font-size:13px}th,td{padding:5px 8px;border-bottom:1px solid #e3e8ef;text-align:left;vertical-align:top}
.num{text-align:right;font-variant-numeric:tabular-nums}.bar div{height:12px;border-radius:0 4px 4px 0}.sw{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:6px}
.formula{color:#5a6a80;font-size:12px;margin:0 0 6px}.meta td:first-child{color:#5a6a80;width:220px}small{color:#5a6a80}
.note{background:#f4f7fb;border-left:4px solid #6250d6;padding:8px 12px}
</style></head><body>
<h1>NetRescue protocol comparison report</h1>
<p>Generated ${esc(new Date().toLocaleString())} from results computed by the NetRescue C simulation engine (API v${result.api_version}).</p>
<p class="note">${result.spec.trials > 1 ? `Aggregate results: ${result.spec.trials} independent trials per protocol${sweep ? ' and sweep value' : ''}; values are mean ± sample standard deviation with Student-t 95% confidence intervals.` : 'Single run per protocol: no statistical aggregation.'}</p>
<h2>Experiment setup</h2>
<table class="meta">
<tr><td>Topology</td><td>${esc(result.topology.description)} (${result.topology.nodes.length} nodes, ${result.topology.links.length} links)</td></tr>
<tr><td>Route</td><td>${esc(result.topology.route_names.join(' → '))}; no-load RTT ${result.topology.base_rtt_ms.toFixed(2)} ms</td></tr>
<tr><td>Protocols</td><td>${result.spec.protocols.map((p) => PROTOCOL_LABEL[p]).join(', ')}</td></tr>
<tr><td>Packets / payload</td><td>${c.packet_count} × ${c.payload_bytes} bytes (+20-byte header per data frame, 20-byte ACK/NAK)</td></tr>
<tr><td>Window</td><td>${c.window_size} for Go-Back-N and Selective Repeat; 1 for Stop-and-Wait</td></tr>
<tr><td>Timeout / retries / time limit</td><td>${(c.timeout_s * 1000).toFixed(0)} ms / ${c.max_retries} / ${c.max_sim_time || 600} s</td></tr>
<tr><td>Offered rate</td><td>${c.packets_per_second} packets/s; congestion control: ${esc(c.congestion_mode)}</td></tr>
<tr><td>Random impairments</td><td>data loss ${pct(c.loss_probability, 0)}, data corruption ${pct(c.corruption_probability, 0)}, ACK loss ${pct(c.ack_loss_probability, 0)}, ACK corruption ${pct(c.ack_corruption_probability, 0)}; NAK ${c.use_nak ? 'on' : 'off'}</td></tr>
<tr><td>Deterministic faults</td><td>${rules}</td></tr>
<tr><td>Sweep</td><td>${sweep ? `${esc(result.spec.sweep.parameter)} = ${result.spec.sweep.values.join(', ')}` : 'none'}</td></tr>
<tr><td>Seeds</td><td>${esc(result.spec.seed_schedule ?? '')}; base ${result.spec.seed_base}, stride ${result.spec.seed_stride}</td></tr>
</table>
<h2>Findings</h2><ul>${result.findings.map((f) => `<li>${esc(f)}</li>`).join('')}</ul>
<h2>Metrics</h2>${metricSections}
<h2>Per-run results</h2>
<table><thead><tr>${sweep ? '<th>Sweep</th>' : ''}<th>Protocol</th><th>Trial</th><th>Seed</th><th>Completed</th><th class="num">PDR</th><th class="num">Goodput</th><th class="num">Latency</th><th class="num">Time</th><th class="num">Retx</th><th>Retransmitted seqs</th></tr></thead><tbody>${runs}</tbody></table>
<h2>Metric definitions</h2><dl>${definitions.map((d) => `<dt><b>${esc(d.name)}</b> (${esc(d.unit)})</dt><dd><code>${esc(d.formula)}</code><br>${esc(d.description)}</dd>`).join('')}</dl>
</body></html>`;
}
