import { useEffect, useState } from 'react';
import { useLab } from '../../state/labStore';
import { PROTOCOL_LABEL } from '../../lib/format';
import { TopologyCanvas } from './TopologyCanvas';
import { EndpointsPanel, IssuesPanel, TopologyPanel } from './TopologyPanel';
import { Inspector } from './Inspector';
import { FaultSettings, ProtocolSettings, QueueSettings, RunControls } from './SimControls';
import { EventLog, ExportSimButton, FaultLedger, FaultSummary, LiveCharts, MetricsGrid, WindowView } from './LiveViews';

/** Drives the engine clock: each animation frame asks the C engine to advance by (real dt x speed). */
function useSimulationLoop() {
  useEffect(() => {
    let raf = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const dt = Math.min((now - last) / 1000, 0.1);
      last = now;
      const s = useLab.getState();
      if (s.sim?.state === 'Running' && !s.busy) void s.step(dt * s.speed);
      s.pruneEffects(now);
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);
}

type Tab = 'events' | 'faults' | 'windows' | 'charts' | 'metrics';

export function SimulationLab() {
  useSimulationLoop();
  const { network, sim, faults, history, linkMode, config } = useLab();
  const [tab, setTab] = useState<Tab>('events');
  const [labels, setLabels] = useState(true);
  if (!network) return null;
  const m = sim?.metrics;
  const name = (id: number) => network.nodes.find((n) => n.id === id)?.name ?? '?';

  return (
    <div className="lab">
      <div className="lab-left">
        <TopologyPanel />
        <EndpointsPanel />
        <Inspector />
        <IssuesPanel network={network} />
      </div>

      <div className="lab-center">
        <section className="panel canvas-panel" aria-label="Network topology">
          <div className="canvas-toolbar">
            <span className="tag">{network.nodes.length} nodes · {network.links.length} links</span>
            {linkMode !== null && <span className="tag warn">Click the node to connect to {name(linkMode)} (Esc: click empty space)</span>}
            <label className="tag" style={{ cursor: 'pointer' }}><input type="checkbox" checked={labels} onChange={(e) => setLabels(e.target.checked)} /> link labels</label>
            <div className="legend" style={{ marginLeft: 'auto', background: 'rgba(10,17,28,.85)', padding: '4px 8px', borderRadius: 6 }}>
              <span><span className="dot" style={{ background: 'var(--data)' }} />DATA</span>
              <span><span className="dot" style={{ background: 'var(--retx)' }} />retransmission</span>
              <span><span className="dot" style={{ background: 'var(--ack)' }} />ACK</span>
              <span><span className="dot" style={{ background: 'var(--nak)' }} />NAK</span>
              <span><span className="dot" style={{ background: 'var(--corrupt)' }} />corrupted</span>
              <span style={{ color: 'var(--critical)' }}>✕ lost</span>
            </div>
          </div>
          <TopologyCanvas showLabels={labels} />
        </section>

        <section className="panel status-strip" aria-label="Simulation status">
          <span>Protocol <b>{PROTOCOL_LABEL[sim?.protocol ?? config.protocol]}</b></span>
          <span>State <b>{sim?.state ?? 'Idle'}</b>{sim && sim.stop_reason && sim.stop_reason !== 'none' ? <span className="muted"> ({sim.stop_reason})</span> : null}</span>
          <span>Sim time <b className="mono">{(sim?.time ?? 0).toFixed(4)} s</b></span>
          {sim?.sender && <span>Window <b className="mono">[{sim.sender.base}, {sim.sender.base + sim.sender.effective_window - 1}]</b> next <b className="mono">{sim.sender.next}</b></span>}
          {m && <span>Delivered <b>{m.packets_delivered}/{m.packets_requested}</b></span>}
          {m && <span>Retx <b>{m.retransmissions}</b></span>}
          {m && <span>Faults <b>{m.faults_total}</b></span>}
          {sim?.in_flight && <span>Frames in network <b>{sim.in_flight.filter((f) => f.kind !== 'cross').length}</b></span>}
          <span style={{ marginLeft: 'auto' }}>{sim && <ExportSimButton />}</span>
        </section>

        <section className="panel">
          <div className="tabs" role="tablist">
            {(['events', 'faults', 'windows', 'charts', 'metrics'] as Tab[]).map((t) => (
              <button key={t} role="tab" aria-selected={tab === t} className={tab === t ? 'active' : ''} onClick={() => setTab(t)}>
                {{ events: 'Event log', faults: `Fault ledger${faults.length ? ` (${faults.length})` : ''}`, windows: 'Sender / receiver windows', charts: 'Live charts', metrics: 'Metrics' }[t]}
              </button>
            ))}
          </div>
          <div className="panel-body">
            {tab === 'events' && <EventLog />}
            {tab === 'faults' && (
              <div className="stack">
                {m && <FaultSummary metrics={m} configured={{ loss: sim?.config?.loss_probability ?? 0, corruption: sim?.config?.corruption_probability ?? 0, ackLoss: sim?.config?.ack_loss_probability ?? 0, ackCorruption: sim?.config?.ack_corruption_probability ?? 0 }} />}
                <FaultLedger faults={faults} />
              </div>
            )}
            {tab === 'windows' && (sim ? <WindowView sim={sim} /> : <div className="empty">Start a transfer to watch the sliding windows.</div>)}
            {tab === 'charts' && <LiveCharts history={history} />}
            {tab === 'metrics' && (m ? <MetricsGrid m={m} /> : <div className="empty">No transfer yet.</div>)}
          </div>
        </section>
      </div>

      <div className="lab-right">
        <RunControls />
        <ProtocolSettings />
        <FaultSettings />
        <QueueSettings />
      </div>
    </div>
  );
}
