import { useEffect, useMemo, useRef, useState } from 'react';
import { CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';
import type { FaultRecord, HistorySample, Metrics, SimState, TraceRecord } from '../../engine/types';
import { useLab } from '../../state/labStore';
import { bps, ms, num, pct, secs, titleCase } from '../../lib/format';
import { downloadText, stamp } from '../../lib/download';

function rowClass(r: TraceRecord): string {
  if (r.type === 'DATA_LOST' || r.type === 'ACK_LOST' || r.type === 'FAILED' || r.type === 'TIMEOUT') return 'fault';
  if (r.type.includes('CORRUPT')) return 'corrupt';
  if (r.type === 'DATA_RETX') return 'retx';
  if (r.type.startsWith('DATA')) return 'data';
  if (r.type.startsWith('ACK') || r.type.startsWith('NAK')) return 'ack';
  return 'info';
}

const FILTERS: Record<string, (r: TraceRecord) => boolean> = {
  all: () => true,
  faults: (r) => /LOST|CORRUPT|TIMEOUT|FAILED|LINK_DOWN|UNDETECTED/.test(r.type),
  retransmissions: (r) => r.type === 'DATA_RETX' || r.type === 'TIMEOUT' || r.type === 'NAK_RECEIVED',
  receiver: (r) => /ACCEPTED|BUFFERED|DELIVERED|DUPLICATE|DISCARDED|OUT_OF_WINDOW|ACK_SEND|NAK_SEND/.test(r.type),
};

export function EventLog() {
  const { trace, traceOffset } = useLab();
  const [filter, setFilter] = useState('all');
  const [seq, setSeq] = useState('');
  const [follow, setFollow] = useState(true);
  const box = useRef<HTMLDivElement>(null);
  const rows = useMemo(() => {
    const seqNum = seq.trim() === '' ? null : Number(seq);
    return trace.filter((r) => FILTERS[filter](r) && (seqNum === null || r.seq === seqNum)).slice(-1500);
  }, [trace, filter, seq]);
  useEffect(() => { if (follow && box.current) box.current.scrollTop = box.current.scrollHeight; }, [rows, follow]);
  return (
    <div>
      <div className="btn-row" style={{ marginBottom: 8, alignItems: 'center' }}>
        <select value={filter} onChange={(e) => setFilter(e.target.value)} style={{ width: 170 }} aria-label="Event filter">
          <option value="all">All events</option><option value="faults">Faults & timeouts</option>
          <option value="retransmissions">Retransmissions</option><option value="receiver">Receiver actions</option>
        </select>
        <input type="text" placeholder="Filter by seq" value={seq} onChange={(e) => setSeq(e.target.value)} style={{ width: 110 }} />
        <label className="small"><input type="checkbox" checked={follow} onChange={(e) => setFollow(e.target.checked)} /> follow</label>
        <span className="muted small">{trace.length + traceOffset} engine events{traceOffset ? ` (first ${traceOffset} trimmed from view)` : ''}</span>
      </div>
      <div className="log" ref={box}>
        {rows.length === 0 && <div className="empty">Start a transfer to see the engine's event trace.</div>}
        {rows.map((r) => (
          <div key={r.i} className={`log-row ${rowClass(r)}`}><span className="t">{r.t.toFixed(4)}s</span><span>{r.text}</span></div>
        ))}
      </div>
    </div>
  );
}

export function FaultLedger({ faults }: { faults: FaultRecord[] }) {
  const { network } = useLab();
  const node = (id: number) => network?.nodes.find((n) => n.id === id)?.name ?? (id < 0 ? '–' : id);
  if (faults.length === 0) return <div className="empty">No faults have occurred yet. Configure probabilities or deterministic rules in “Fault injection”.</div>;
  return (
    <div className="table-wrap" style={{ maxHeight: 320 }}>
      <table className="data">
        <thead><tr><th className="num">time s</th><th>fault</th><th>source</th><th>frame</th><th className="num">seq</th><th className="num">attempt</th><th>at node</th><th className="num">link</th><th>detected</th><th>protocol response</th><th>recovered</th></tr></thead>
        <tbody>
          {faults.slice(-500).map((f) => (
            <tr key={f.i}>
              <td className="num">{f.t.toFixed(4)}</td><td>{titleCase(f.type)}</td><td className="muted">{f.source.replace('_', ' ')}</td>
              <td>{f.frame_kind.toUpperCase()}</td><td className="num">{f.seq}</td><td className="num">{f.attempt}</td><td>{node(f.node)}</td><td className="num">{f.link < 0 ? '–' : f.link}</td>
              <td>{f.detected ? <span className="tag good">yes{f.detected_at !== null ? ` @${f.detected_at.toFixed(3)}` : ''}</span> : <span className="tag">no</span>}</td>
              <td className="small">{f.response.replace(/_/g, ' ')}</td>
              <td>{f.recovered ? <span className="tag good">yes{f.recovered_at !== null ? ` @${f.recovered_at.toFixed(3)}` : ''}</span> : <span className="tag bad">no</span>}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function FaultSummary({ metrics, configured }: { metrics: Metrics; configured?: { loss: number; corruption: number; ackLoss: number; ackCorruption: number } }) {
  const types = Object.entries(metrics.faults);
  return (
    <div className="table-wrap">
      <table className="data">
        <thead><tr><th>fault type</th>{configured && <th className="num">configured p</th>}<th className="num">injected</th><th className="num">detected</th><th className="num">recovered</th><th className="num">no impact</th><th className="num">unrecovered</th></tr></thead>
        <tbody>
          {types.map(([type, c]) => {
            const p = configured ? ({ data_loss: configured.loss, data_corruption: configured.corruption, ack_loss: configured.ackLoss, ack_corruption: configured.ackCorruption } as Record<string, number>)[type] : undefined;
            return (
              <tr key={type}><td>{titleCase(type)}</td>{configured && <td className="num">{p === undefined ? '–' : pct(p, 0)}</td>}
                <td className="num">{c.injected}</td><td className="num">{c.detected}</td><td className="num">{c.recovered}</td><td className="num">{c.no_impact}</td>
                <td className="num" style={{ color: c.unrecovered ? 'var(--critical)' : undefined }}>{c.unrecovered}</td></tr>
            );
          })}
        </tbody>
      </table>
      <p className="small muted">Configured probabilities are per transmission; “injected” counts what the engine actually did. Per-link loss and congestion drops are counted under data/ACK loss and queue overflow.</p>
    </div>
  );
}

export function WindowView({ sim }: { sim: SimState }) {
  if (!sim.sender_map || !sim.sender || !sim.receiver) return null;
  const { base, next, effective_window } = sim.sender;
  const rbase = sim.receiver.base;
  const window = sim.config?.window_size ?? effective_window;
  const sLabel: Record<string, string> = { n: 'not sent', o: 'sent, awaiting ACK', a: 'acknowledged', f: 'failed' };
  return (
    <div className="window-view">
      <div className="small" style={{ marginBottom: 6 }}>
        <b>Sender</b> base={base} next={next} window={effective_window}{sim.config?.congestion_mode === 'window_aimd' ? ` (cwnd ${num(sim.sender.cwnd, 2)})` : ''}
        {sim.protocol === 'go_back_n' && <span className="muted"> · single timer {sim.sender.gbn_timer_running ? 'running' : 'stopped'}</span>}
      </div>
      <div className="cells" role="list" aria-label="Sender window">
        {[...sim.sender_map].map((c, i) => (
          <div key={i} role="listitem" className={`cell ${c === 'n' ? '' : c} ${i >= base && i < base + effective_window ? 'win' : ''}`}
            title={`seq ${i}: ${sLabel[c]}, ${sim.attempts_map?.[i] ?? 0} transmission(s)`}>{i}</div>
        ))}
      </div>
      <div className="small" style={{ margin: '10px 0 6px' }}>
        <b>Receiver</b> next expected={rbase}{sim.protocol === 'selective_repeat' ? ` · receive window [${rbase}, ${rbase + window - 1}]` : ' · accepts only the next expected frame'}
      </div>
      <div className="cells" role="list" aria-label="Receiver state">
        {[...(sim.receiver_map ?? '')].map((c, i) => (
          <div key={i} role="listitem" className={`cell ${c === '-' ? '' : c} ${sim.protocol === 'selective_repeat' && i >= rbase && i < rbase + window ? 'win' : ''}`}
            title={`seq ${i}: ${c === 'd' ? 'delivered to application' : c === 'b' ? 'buffered out of order' : 'not received'}`}>{i}</div>
        ))}
      </div>
      <div className="legend" style={{ marginTop: 10 }}>
        <span><i className="cell o" style={{ width: 14, height: 12 }} /> outstanding</span><span><i className="cell a" style={{ width: 14, height: 12 }} /> acked / delivered</span>
        <span><i className="cell b" style={{ width: 14, height: 12 }} /> buffered (SR)</span><span><i className="cell win" style={{ width: 14, height: 12 }} /> inside window</span>
      </div>
    </div>
  );
}

const CHARTS: { key: keyof HistorySample; title: string; format: (v: number) => string; note: string }[] = [
  { key: 'delivery_ratio', title: 'Delivered (fraction of packets)', format: (v) => pct(v), note: 'delivered / requested' },
  { key: 'goodput_mbps', title: 'Goodput (Mbit/s, cumulative)', format: (v) => `${v.toFixed(3)} Mbit/s`, note: 'delivered payload bits / elapsed time' },
  { key: 'latency_ms', title: 'Average end-to-end latency (ms)', format: (v) => `${v.toFixed(1)} ms`, note: 'first send → in-order delivery' },
  { key: 'retransmissions', title: 'Retransmissions (cumulative)', format: (v) => v.toFixed(0), note: 'data frames resent' },
  { key: 'queue_utilization', title: 'Route queue utilisation', format: (v) => pct(v), note: 'fullest forwarding queue on the route' },
  { key: 'window', title: 'Effective window / rate', format: (v) => v.toFixed(1), note: 'window (frames); shrinks under window AIMD' },
];

function ChartTip({ active, payload, label, format }: { active?: boolean; payload?: { value: number }[]; label?: number; format: (v: number) => string }) {
  if (!active || !payload?.length) return null;
  return <div className="chart-tip"><div className="muted">t = {Number(label).toFixed(2)} s</div><b>{format(payload[0].value)}</b></div>;
}

export function LiveCharts({ history }: { history: HistorySample[] }) {
  if (history.length < 2) return <div className="empty">Charts appear once the engine has recorded samples (every 0.1 s of simulated time).</div>;
  return (
    <div className="chart-grid" style={{ gridTemplateColumns: 'repeat(auto-fill, minmax(280px, 1fr))' }}>
      {CHARTS.map((c) => (
        <div className="chart-card" key={c.key}>
          <h4>{c.title}</h4><div className="formula">{c.note}</div>
          <ResponsiveContainer width="100%" height={150}>
            <LineChart data={history} margin={{ top: 4, right: 8, bottom: 0, left: -12 }}>
              <CartesianGrid stroke="#1d2b40" vertical={false} />
              <XAxis dataKey="t" type="number" domain={['dataMin', 'dataMax']} tick={{ fill: '#7487a0', fontSize: 10 }} tickFormatter={(v) => `${Number(v).toFixed(1)}s`} stroke="#26364d" />
              <YAxis tick={{ fill: '#7487a0', fontSize: 10 }} stroke="#26364d" width={48} />
              <Tooltip content={<ChartTip format={c.format} />} />
              <Line type="monotone" dataKey={c.key} stroke="#3fa7ff" strokeWidth={2} dot={false} isAnimationActive={false} />
            </LineChart>
          </ResponsiveContainer>
        </div>
      ))}
    </div>
  );
}

export function MetricsGrid({ m }: { m: Metrics }) {
  const tiles: [string, string, string?][] = [
    ['Delivery ratio', pct(m.delivery_ratio), `${m.packets_delivered}/${m.packets_requested} unique`],
    ['Goodput', bps(m.goodput_bps), 'useful payload'],
    ['Throughput', bps(m.throughput_bps), 'all bits on the wire'],
    ['Avg latency', ms(m.latency_avg_ms), `min ${ms(m.latency_min_ms)} · max ${ms(m.latency_max_ms)}`],
    ['Elapsed', secs(m.elapsed_s), m.completed ? 'completed' : `stop: ${m.stop_reason}`],
    ['Retransmissions', String(m.retransmissions), `timeout ${m.retx_timeout} · NAK ${m.retx_nak} · go-back ${m.retx_goback}`],
    ['Retransmission rate', num(m.retransmission_rate, 3), 'retx / first transmissions'],
    ['Unnecessary retx', String(m.retx_unnecessary), 'receiver already had it'],
    ['Efficiency', pct(m.efficiency), 'payload bits / wire bits'],
    ['Timeouts', String(m.timeouts), `avg RTT ${ms(m.rtt_avg_ms)}`],
    ['ACKs / NAKs sent', `${m.acks_sent} / ${m.naks_sent}`, `${m.dup_acks_sent} duplicate ACKs`],
    ['Duplicates at receiver', String(m.duplicates_received), `${m.out_of_order_received} out of order`],
    ['Queue drops', String(m.queue_drops), `data ${m.data_queue_drops} · ACK ${m.ack_queue_drops}`],
    ['Peak queue', String(m.queue_max_occupancy), `avg ${num(m.queue_avg_occupancy, 2)} frames`],
    ['Faults', `${m.faults_recovered}/${m.faults_total}`, 'recovered / injected'],
    ['Congestion events', `${m.congestion_decreases}↓ ${m.congestion_increases}↑`, `min window ${num(m.min_window, 1)} · min rate ${num(m.min_rate, 1)}`],
  ];
  return <div className="kpis">{tiles.map(([k, v, s]) => <div className="kpi" key={k}><div className="k">{k}</div><div className="v">{v}</div>{s && <div className="s">{s}</div>}</div>)}</div>;
}

export function ExportSimButton() {
  const { engine, notify } = useLab();
  return (
    <button className="btn sm" onClick={async () => {
      if (!engine) return;
      try {
        const r = await engine.call<{ trace_csv: string }>('sim_export');
        downloadText(`netrescue-trace-${stamp()}.csv`, r.trace_csv, 'text/csv');
        downloadText(`netrescue-run-${stamp()}.json`, JSON.stringify(r, null, 2), 'application/json');
      } catch (e) { notify('error', (e as Error).message); }
    }}>Export trace (CSV + JSON)</button>
  );
}
