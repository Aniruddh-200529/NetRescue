// Presents an experiment result exactly as returned by the C engine (run_experiment). Charts and tables
// re-arrange the engine's per-run metrics and aggregates (mean / stddev / 95% CI); nothing is recomputed here.
import { useMemo, useState } from 'react';
import { Bar, BarChart, CartesianGrid, Cell, ErrorBar, Legend, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';
import type { ExperimentAggregate, ExperimentResult, MetricDefinition, ProtocolId, Stat } from '../../engine/types';
import { bps, ms, num, pct, PROTOCOL_COLOR, PROTOCOL_LABEL, PROTOCOL_SHORT, secs, sweepLabel, titleCase } from '../../lib/format';
import { downloadText, stamp } from '../../lib/download';
import { buildReport } from '../../lib/report';

type Fmt = (v: number | null) => string;
const compact = new Intl.NumberFormat('en', { notation: 'compact', maximumFractionDigits: 2 });
/** Axis ticks stay short; the unit is in the chart title and the full value in tooltips and labels. */
const axisTick = (key: string) => (v: number) => (key === 'delivery_ratio' || key === 'efficiency' ? `${Math.round(v * 100)}%` : compact.format(v));
export const CHART_METRICS: { key: string; title: string; format: Fmt; def?: string }[] = [
  { key: 'goodput_bps', title: 'Goodput (bit/s)', format: bps, def: 'goodput_bps' },
  { key: 'throughput_bps', title: 'Throughput, all bits on the wire (bit/s)', format: bps, def: 'throughput_bps' },
  { key: 'latency_avg_ms', title: 'Average end-to-end latency (ms)', format: ms, def: 'latency_avg_ms' },
  { key: 'completion_time_s', title: 'Completion time (s)', format: secs, def: 'completion_time_s' },
  { key: 'retransmissions', title: 'Data retransmissions', format: (v) => num(v, 1), def: 'retransmissions' },
  { key: 'delivery_ratio', title: 'Packet delivery ratio (%)', format: pct, def: 'delivery_ratio' },
  { key: 'efficiency', title: 'Protocol efficiency (%)', format: pct, def: 'efficiency' },
  { key: 'retx_unnecessary', title: 'Unnecessary retransmissions', format: (v) => num(v, 1), def: 'retx_unnecessary' },
  { key: 'queue_drops', title: 'Queue drops', format: (v) => num(v, 1), def: 'queue_drops' },
  { key: 'queue_avg_occupancy', title: 'Bottleneck queue occupancy (time-avg)', format: (v) => num(v, 2), def: 'queue_avg_occupancy' },
];

function statText(s: Stat | undefined, format: Fmt, trials: number): string {
  if (!s || s.n === 0) return 'n/a';
  if (trials <= 1) return format(s.mean);
  return `${format(s.mean)} ± ${format(s.stddev)}`;
}

function Tip({ active, payload, label, format, sweep }: { active?: boolean; payload?: { name: string; value: number; color?: string; payload: Record<string, unknown> }[]; label?: string; format: Fmt; sweep: boolean }) {
  if (!active || !payload?.length) return null;
  return (
    <div className="chart-tip">
      <div className="muted">{label}</div>
      {payload.map((p) => {
        const protocol = (sweep ? p.name : p.payload.protocol) as ProtocolId;
        const ci = (sweep ? p.payload[`${protocol}_ci`] : p.payload.ci) as number | undefined;
        const n = (sweep ? p.payload[`${protocol}_n`] : p.payload.n) as number | undefined;
        return (
          <div className="row" key={p.name}>
            <span><span className="swatch" style={{ background: PROTOCOL_COLOR[protocol] }} />{PROTOCOL_LABEL[protocol]}</span>
            <b>{format(p.value)}{ci ? <span className="muted"> ±{format(ci)} (95% CI, n={n})</span> : null}</b>
          </div>
        );
      })}
    </div>
  );
}

function MetricChart({ result, metric, definition }: { result: ExperimentResult; metric: (typeof CHART_METRICS)[number]; definition?: MetricDefinition }) {
  const sweep = result.spec.sweep.parameter !== 'none';
  const protocols = result.spec.protocols;
  const showError = result.spec.trials > 1;
  const agg = (si: number, p: ProtocolId) => result.aggregates.find((a) => a.sweep_index === si && a.protocol === p);

  if (!sweep) {
    const data = protocols.map((p) => { const s = agg(0, p)?.stats[metric.key]; return { protocol: p, name: PROTOCOL_SHORT[p], mean: s?.mean ?? null, ci: s?.ci95 ?? 0, n: s?.n ?? 0 }; });
    return (
      <ResponsiveContainer width="100%" height={190}>
        <BarChart data={data} margin={{ top: 18, right: 8, bottom: 0, left: 4 }} barCategoryGap="28%">
          <CartesianGrid stroke="#1d2b40" vertical={false} />
          <XAxis dataKey="name" tick={{ fill: '#a9b7ca', fontSize: 11 }} stroke="#26364d" />
          <YAxis tick={{ fill: '#7487a0', fontSize: 10 }} stroke="#26364d" width={44} tickFormatter={axisTick(metric.key)} />
          <Tooltip cursor={{ fill: 'rgba(255,255,255,0.04)' }} content={<Tip format={metric.format} sweep={false} />} />
          <Bar dataKey="mean" radius={[4, 4, 0, 0]} isAnimationActive={false} label={{ position: 'top', fill: '#a9b7ca', fontSize: 10, formatter: (v: number) => metric.format(v) }}>
            {data.map((d) => <Cell key={d.protocol} fill={PROTOCOL_COLOR[d.protocol]} />)}
            {showError && <ErrorBar dataKey="ci" width={6} stroke="#e8eef6" strokeWidth={1.2} />}
          </Bar>
        </BarChart>
      </ResponsiveContainer>
    );
  }
  const values = result.spec.sweep.values;
  const data = values.map((v, si) => {
    const row: Record<string, unknown> = { x: sweepLabel(result.spec.sweep.parameter, v) };
    for (const p of protocols) { const s = agg(si, p)?.stats[metric.key]; row[p] = s?.mean ?? null; row[`${p}_ci`] = s?.ci95 ?? 0; row[`${p}_n`] = s?.n ?? 0; }
    return row;
  });
  void definition;
  return (
    <ResponsiveContainer width="100%" height={210}>
      <LineChart data={data} margin={{ top: 8, right: 10, bottom: 0, left: 4 }}>
        <CartesianGrid stroke="#1d2b40" vertical={false} />
        <XAxis dataKey="x" tick={{ fill: '#a9b7ca', fontSize: 11 }} stroke="#26364d" />
        <YAxis tick={{ fill: '#7487a0', fontSize: 10 }} stroke="#26364d" width={44} tickFormatter={axisTick(metric.key)} />
        <Tooltip content={<Tip format={metric.format} sweep />} />
        <Legend wrapperStyle={{ fontSize: 11, color: '#a9b7ca' }} formatter={(value: string) => PROTOCOL_LABEL[value as ProtocolId]} />
        {protocols.map((p) => (
          <Line key={p} type="monotone" dataKey={p} name={p} stroke={PROTOCOL_COLOR[p]} strokeWidth={2} dot={{ r: 4, strokeWidth: 2, stroke: '#111a27' }} isAnimationActive={false}>
            {showError && <ErrorBar dataKey={`${p}_ci`} width={5} stroke={PROTOCOL_COLOR[p]} strokeWidth={1.2} />}
          </Line>
        ))}
      </LineChart>
    </ResponsiveContainer>
  );
}

function AggregateTable({ result, columns }: { result: ExperimentResult; columns: { key: string; label: string; format: Fmt }[] }) {
  const sweep = result.spec.sweep.parameter !== 'none';
  return (
    <div className="table-wrap">
      <table className="data">
        <thead><tr>{sweep && <th>{titleCase(result.spec.sweep.parameter)}</th>}<th>Protocol</th><th className="num">Completed</th>{columns.map((c) => <th className="num" key={c.key}>{c.label}</th>)}</tr></thead>
        <tbody>
          {result.aggregates.map((a: ExperimentAggregate) => (
            <tr key={`${a.sweep_index}-${a.protocol}`}>
              {sweep && <td>{sweepLabel(result.spec.sweep.parameter, a.sweep_value)}</td>}
              <td><span className="swatch" style={{ background: PROTOCOL_COLOR[a.protocol] }} />{PROTOCOL_LABEL[a.protocol]}</td>
              <td className="num"><span className={`tag ${a.completed === a.runs ? 'good' : 'bad'}`}>{a.completed}/{a.runs}</span></td>
              {columns.map((c) => <td className="num" key={c.key} title={a.stats[c.key]?.ci95 != null ? `95% CI ±${c.format(a.stats[c.key].ci95)} (n=${a.stats[c.key].n})` : undefined}>{statText(a.stats[c.key], c.format, result.spec.trials)}</td>)}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function TraceViewer({ result }: { result: ExperimentResult }) {
  const [index, setIndex] = useState(0);
  const [filter, setFilter] = useState('');
  if (result.captures.length === 0) return <div className="empty">No traces were captured. Enable “Capture the full event trace” before running.</div>;
  const capture = result.captures[Math.min(index, result.captures.length - 1)];
  const rows = capture.trace.filter((r) => !filter || r.type.includes(filter.toUpperCase()) || String(r.seq) === filter);
  return (
    <div>
      <div className="btn-row" style={{ marginBottom: 8 }}>
        <select value={index} onChange={(e) => setIndex(Number(e.target.value))} style={{ width: 320 }} aria-label="Captured run">
          {result.captures.map((c, i) => <option key={c.run_index} value={i}>{PROTOCOL_LABEL[c.protocol]}{result.spec.sweep.parameter !== 'none' ? ` @ ${sweepLabel(result.spec.sweep.parameter, result.spec.sweep.values[c.sweep_index])}` : ''} (trial 0, {c.trace_total} events)</option>)}
        </select>
        <input type="text" placeholder="Filter: type (e.g. RETX, LOST) or seq" value={filter} onChange={(e) => setFilter(e.target.value)} style={{ width: 260 }} />
      </div>
      <div className="log" style={{ maxHeight: 360 }}>
        {rows.slice(0, 2500).map((r) => <div key={r.i} className={`log-row ${/LOST|TIMEOUT/.test(r.type) ? 'fault' : /CORRUPT/.test(r.type) ? 'corrupt' : r.type === 'DATA_RETX' ? 'retx' : r.type.startsWith('DATA') ? 'data' : /ACK|NAK/.test(r.type) ? 'ack' : 'info'}`}><span className="t">{r.t.toFixed(4)}s</span><span>{r.text}</span></div>)}
      </div>
      {capture.trace_total > capture.trace.length && <p className="small muted">Showing the first {capture.trace.length} of {capture.trace_total} events.</p>}
    </div>
  );
}

type Tab = 'charts' | 'table' | 'faults' | 'queues' | 'evidence' | 'trials' | 'trace';

export function Results({ result, csv, definitions }: { result: ExperimentResult; csv: string; definitions: MetricDefinition[] }) {
  const [tab, setTab] = useState<Tab>('charts');
  const sweep = result.spec.sweep.parameter !== 'none';
  const aggregate = result.spec.trials > 1;
  const def = (key?: string) => definitions.find((d) => d.key === key);
  const evidenceRuns = useMemo(() => result.runs.filter((r) => r.trial === 0), [result]);

  const exportBase = `netrescue-experiment-${stamp()}`;
  return (
    <div className="stack">
      <section className="panel">
        <div className="panel-head">
          <h3>Results</h3>
          <div className="btn-row">
            <button className="btn sm" onClick={() => downloadText(`${exportBase}.csv`, csv, 'text/csv')}>Export CSV</button>
            <button className="btn sm" onClick={() => downloadText(`${exportBase}.json`, JSON.stringify(result, null, 2), 'application/json')}>Export JSON</button>
            <button className="btn sm violet" onClick={() => downloadText(`${exportBase}-report.html`, buildReport(result, definitions), 'text/html')}>Download report</button>
          </div>
        </div>
        <div className="panel-body small">
          <div className="btn-row" style={{ marginBottom: 6 }}>
            <span className={`tag ${aggregate ? 'warn' : ''}`}>{aggregate ? `Aggregate of ${result.spec.trials} independent trials (mean ± sample std. dev.; 95% CI in tooltips)` : 'Single run per protocol (no statistics)'}</span>
            {sweep && <span className="tag">Sweep: {titleCase(result.spec.sweep.parameter)} = {result.spec.sweep.values.map((v) => sweepLabel(result.spec.sweep.parameter, v)).join(', ')}</span>}
            <span className="tag">{result.runs.length} engine runs</span>
          </div>
          <div><span className="muted">Topology:</span> {result.topology.description} · <span className="muted">route</span> {result.topology.route_names.join(' → ')} · <span className="muted">no-load RTT</span> {result.topology.base_rtt_ms.toFixed(2)} ms</div>
          <div><span className="muted">Seeds:</span> {result.spec.seed_schedule} (base {result.spec.seed_base}, stride {result.spec.seed_stride})</div>
          <div><span className="muted">Per-run settings:</span> {result.spec.config.packet_count} packets × {result.spec.config.payload_bytes} B, window {result.spec.config.window_size} (SW = 1), timeout {(result.spec.config.timeout_s * 1000).toFixed(0)} ms, max retries {result.spec.config.max_retries}, loss {pct(result.spec.config.loss_probability, 0)}, corruption {pct(result.spec.config.corruption_probability, 0)}, ACK loss {pct(result.spec.config.ack_loss_probability, 0)}, ACK corruption {pct(result.spec.config.ack_corruption_probability, 0)}{result.spec.scenario.length ? `, ${result.spec.scenario.length} deterministic fault rule(s)` : ''}</div>
        </div>
      </section>

      <section className="panel">
        <div className="panel-head"><h3>What the measurements show</h3><span className="muted small">generated by the C engine from these results</span></div>
        <div className="panel-body"><ul className="findings">{result.findings.map((f, i) => <li key={i}>{f}</li>)}</ul></div>
      </section>

      <section className="panel">
        <div className="tabs" role="tablist">
          {([['charts', 'Charts'], ['table', 'Results table'], ['faults', 'Faults & recovery'], ['queues', 'Queues & congestion'], ['evidence', 'Retransmission evidence'], ['trials', `Per-trial results (${result.runs.length})`], ['trace', 'Captured traces']] as [Tab, string][]).map(([t, label]) => (
            <button key={t} role="tab" aria-selected={tab === t} className={tab === t ? 'active' : ''} onClick={() => setTab(t)}>{label}</button>
          ))}
        </div>
        <div className="panel-body">
          {tab === 'charts' && (
            <div className="chart-grid">
              {CHART_METRICS.map((m) => (
                <div className="chart-card" key={m.key}>
                  <h4>{m.title}</h4>
                  <div className="formula" title={def(m.def)?.description}>{def(m.def)?.formula ?? ''}</div>
                  <MetricChart result={result} metric={m} definition={def(m.def)} />
                </div>
              ))}
            </div>
          )}
          {tab === 'table' && <AggregateTable result={result} columns={[
            { key: 'delivery_ratio', label: 'PDR', format: pct }, { key: 'goodput_bps', label: 'Goodput', format: bps },
            { key: 'throughput_bps', label: 'Throughput', format: bps }, { key: 'latency_avg_ms', label: 'Avg latency', format: ms },
            { key: 'completion_time_s', label: 'Completion', format: secs }, { key: 'retransmissions', label: 'Retx', format: (v) => num(v, 1) },
            { key: 'retransmission_rate', label: 'Retx rate', format: (v) => num(v, 3) }, { key: 'efficiency', label: 'Efficiency', format: pct },
            { key: 'timeouts', label: 'Timeouts', format: (v) => num(v, 1) }, { key: 'packets_undelivered', label: 'Undelivered', format: (v) => num(v, 1) },
          ]} />}
          {tab === 'faults' && <AggregateTable result={result} columns={[
            { key: 'faults_injected', label: 'Faults injected', format: (v) => num(v, 1) }, { key: 'data_losses', label: 'Data losses', format: (v) => num(v, 1) },
            { key: 'corruptions_detected', label: 'Corruptions detected', format: (v) => num(v, 1) }, { key: 'ack_losses', label: 'ACK losses', format: (v) => num(v, 1) },
            { key: 'queue_drops', label: 'Queue drops', format: (v) => num(v, 1) }, { key: 'timeouts', label: 'Timeouts', format: (v) => num(v, 1) },
            { key: 'faults_recovered', label: 'Recovered', format: (v) => num(v, 1) }, { key: 'faults_unrecovered', label: 'Unrecovered', format: (v) => num(v, 1) },
            { key: 'duplicates_received', label: 'Duplicates suppressed', format: (v) => num(v, 1) }, { key: 'packets_undelivered', label: 'Left undelivered', format: (v) => num(v, 1) },
          ]} />}
          {tab === 'queues' && <AggregateTable result={result} columns={[
            { key: 'queue_avg_occupancy', label: 'Bottleneck avg queue', format: (v) => num(v, 2) }, { key: 'queue_max_occupancy', label: 'Peak queue', format: (v) => num(v, 1) },
            { key: 'queue_drops', label: 'Queue drops', format: (v) => num(v, 1) }, { key: 'congestion_decreases', label: 'CC decreases', format: (v) => num(v, 1) },
            { key: 'latency_max_ms', label: 'Max latency', format: ms }, { key: 'completion_time_s', label: 'Completion', format: secs },
          ]} />}
          {tab === 'evidence' && (
            <div className="table-wrap">
              <p className="small muted" style={{ marginTop: 0 }}>Sequence numbers each protocol had to retransmit in trial 0 (from the sender's per-packet transmission counts), with the split of retransmission causes.</p>
              <table className="data">
                <thead><tr>{sweep && <th>{titleCase(result.spec.sweep.parameter)}</th>}<th>Protocol</th><th className="num">Retx</th><th className="num">timeout / NAK / go-back</th><th className="num">unnecessary</th><th>Retransmitted sequence numbers (×count when &gt; 1)</th></tr></thead>
                <tbody>
                  {evidenceRuns.map((r) => (
                    <tr key={r.index}>
                      {sweep && <td>{sweepLabel(result.spec.sweep.parameter, r.sweep_value)}</td>}
                      <td><span className="swatch" style={{ background: PROTOCOL_COLOR[r.protocol] }} />{PROTOCOL_LABEL[r.protocol]}</td>
                      <td className="num">{r.metrics.retransmissions}</td>
                      <td className="num">{r.metrics.retx_timeout} / {r.metrics.retx_nak} / {r.metrics.retx_goback}</td>
                      <td className="num">{r.metrics.retx_unnecessary}</td>
                      <td className="mono">{r.retransmitted.length ? r.retransmitted.map((x) => (x.count > 1 ? `${x.seq}×${x.count}` : x.seq)).join(', ') : '—'}{r.retransmitted_sequences > r.retransmitted.length ? ` … (${r.retransmitted_sequences} packets)` : ''}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {tab === 'trials' && (
            <div className="table-wrap" style={{ maxHeight: 480 }}>
              <table className="data">
                <thead><tr>{sweep && <th>{titleCase(result.spec.sweep.parameter)}</th>}<th>Protocol</th><th className="num">Trial</th><th className="num">Seed</th><th>Done</th><th className="num">PDR</th><th className="num">Goodput</th><th className="num">Latency</th><th className="num">Time</th><th className="num">Retx</th><th className="num">Faults</th><th className="num">Queue drops</th></tr></thead>
                <tbody>
                  {result.runs.map((r) => (
                    <tr key={r.index}>
                      {sweep && <td>{sweepLabel(result.spec.sweep.parameter, r.sweep_value)}</td>}
                      <td>{PROTOCOL_SHORT[r.protocol]}</td><td className="num">{r.trial}</td><td className="num mono">{r.seed}</td>
                      <td>{r.metrics.completed ? <span className="tag good">yes</span> : <span className="tag bad">{r.metrics.stop_reason}</span>}</td>
                      <td className="num">{pct(r.metrics.delivery_ratio)}</td><td className="num">{bps(r.metrics.goodput_bps)}</td><td className="num">{ms(r.metrics.latency_avg_ms)}</td>
                      <td className="num">{secs(r.metrics.elapsed_s)}</td><td className="num">{r.metrics.retransmissions}</td><td className="num">{r.metrics.faults_total}</td><td className="num">{r.metrics.queue_drops}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {tab === 'trace' && <TraceViewer result={result} />}
        </div>
      </section>
    </div>
  );
}
