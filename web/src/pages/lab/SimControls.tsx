import type { ArqConfig, CongestionMode, ProtocolId } from '../../engine/types';
import { NumberField, Panel, ProbabilityField, SelectField } from '../../components/Fields';
import { ScenarioEditor } from '../../components/ScenarioEditor';
import { useLab } from '../../state/labStore';
import { PROTOCOL_LABEL, PROTOCOLS } from '../../lib/format';

const SPEEDS = [0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5];

export function RunControls() {
  const { sim, busy, speed, start, simCommand, setSpeed, network } = useLab();
  const state = sim?.state ?? 'Idle';
  const active = !!sim?.active;
  const blocked = !!network?.issues.some((i) => i.severity === 'error');
  return (
    <Panel title="Simulation control" actions={<span className={`tag ${state === 'Complete' ? 'good' : state === 'Failed' ? 'bad' : state === 'Running' ? 'warn' : ''}`}>{state}</span>}>
      <div className="btn-row" style={{ marginBottom: 10 }}>
        <button className="btn primary" disabled={busy || blocked} onClick={() => void start()} title={active ? 'Restart with the current settings' : 'Start a transfer'}>{active ? '↻ Restart' : '▶ Start'}</button>
        {state === 'Running' && <button className="btn" onClick={() => void simCommand('sim_pause')}>❚❚ Pause</button>}
        {state === 'Paused' && <button className="btn" onClick={() => void simCommand('sim_resume')}>▶ Resume</button>}
        <button className="btn" disabled={!active} onClick={() => void simCommand('sim_step_event')} title="Process the next engine event (pauses first)">⏭ Step</button>
        <button className="btn" disabled={!active} onClick={() => void simCommand('sim_stop')}>■ Stop</button>
        <button className="btn ghost" disabled={!sim} onClick={() => void simCommand('sim_reset')}>Reset</button>
      </div>
      <div className="field">
        <label>Speed<b className="mono">{speed}× real time</b></label>
        <input type="range" min={0} max={SPEEDS.length - 1} step={1} value={Math.max(0, SPEEDS.indexOf(speed))} aria-label="Simulation speed"
          onChange={(e) => setSpeed(SPEEDS[Number(e.target.value)])} />
        <span className="hint">Simulated seconds per real second. Step works while paused and advances to the next visible protocol event.</span>
      </div>
      {blocked && <div className="issue error">Fix the validation errors before starting.</div>}
      {active && <div className="small muted">Settings below apply on the next Start/Restart. Link failures and link property edits apply immediately.</div>}
    </Panel>
  );
}

export function ProtocolSettings() {
  const { config, setConfig } = useLab();
  const set = (patch: Partial<ArqConfig>) => setConfig(patch);
  return (
    <Panel title="Protocol & traffic">
      <SelectField<ProtocolId> label="ARQ protocol" value={config.protocol} onChange={(protocol) => set({ protocol })}
        options={PROTOCOLS.map((p) => ({ value: p, label: PROTOCOL_LABEL[p] }))} />
      <div className="grid2">
        <NumberField label="Packets" value={config.packet_count} min={1} max={1024} onChange={(packet_count) => set({ packet_count })} />
        <NumberField label="Payload" suffix="bytes" value={config.payload_bytes} min={1} max={65000} onChange={(payload_bytes) => set({ payload_bytes })} />
        <NumberField label="Window size" value={config.protocol === 'stop_and_wait' ? 1 : config.window_size} min={1} max={1024}
          disabled={config.protocol === 'stop_and_wait'} onChange={(window_size) => set({ window_size })} />
        <NumberField label="Timeout" suffix="ms" value={Math.round(config.timeout_s * 1000)} min={1} max={60000} onChange={(v) => set({ timeout_s: v / 1000 })} />
        <NumberField label="Max retries" value={config.max_retries} min={0} max={1000} onChange={(max_retries) => set({ max_retries })} />
        <NumberField label="Offered rate" suffix="pkt/s" value={config.packets_per_second} min={0} max={100000} onChange={(packets_per_second) => set({ packets_per_second })} />
      </div>
    </Panel>
  );
}

export function FaultSettings() {
  const { config, setConfig, scenario, setScenario, network } = useLab();
  return (
    <Panel title="Fault injection">
      <div className="grid2">
        <ProbabilityField label="Data loss" value={config.loss_probability} onChange={(loss_probability) => setConfig({ loss_probability })} />
        <ProbabilityField label="Data corruption" value={config.corruption_probability} onChange={(corruption_probability) => setConfig({ corruption_probability })} />
        <ProbabilityField label="ACK loss" value={config.ack_loss_probability} onChange={(ack_loss_probability) => setConfig({ ack_loss_probability })} />
        <ProbabilityField label="ACK corruption" value={config.ack_corruption_probability} onChange={(ack_corruption_probability) => setConfig({ ack_corruption_probability })} />
      </div>
      <div className="grid2">
        <NumberField label="Random seed" value={config.seed} min={0} max={4294967295} onChange={(seed) => setConfig({ seed })} />
        <NumberField label="Bits per corruption" value={config.corruption_bits} min={1} max={64} onChange={(corruption_bits) => setConfig({ corruption_bits })} />
      </div>
      <label className="small" style={{ display: 'flex', gap: 6, alignItems: 'center', marginBottom: 10 }}>
        <input type="checkbox" checked={config.use_nak} onChange={(e) => setConfig({ use_nak: e.target.checked })} /> Receiver sends NAK on checksum failure
      </label>
      <ScenarioEditor rules={scenario} onChange={setScenario} links={network?.links ?? []} nodes={network?.nodes ?? []} />
    </Panel>
  );
}

export function QueueSettings() {
  const { config, setConfig, networkCommand, sim } = useLab();
  const locked = !!sim?.active;
  return (
    <Panel title="Queues & congestion">
      <SelectField<CongestionMode> label="Congestion control" value={config.congestion_mode} onChange={(congestion_mode) => setConfig({ congestion_mode })}
        options={[{ value: 'none', label: 'None' }, { value: 'rate_aimd', label: 'Rate AIMD (queue-overflow signal)' }, { value: 'window_aimd', label: 'Window AIMD (timeout signal)' }]} />
      <div className="grid2">
        <NumberField label="Cross traffic" suffix="frames/s" value={config.cross_traffic_pps} min={0} max={100000} onChange={(cross_traffic_pps) => setConfig({ cross_traffic_pps })}
          hint="Background load on the first forwarding hop" />
        <NumberField label="Time limit" suffix="s (0 = 600)" value={config.max_sim_time} min={0} max={100000} onChange={(max_sim_time) => setConfig({ max_sim_time })} />
      </div>
      <div className="small muted" style={{ marginBottom: 6 }}>Apply to every node / link of the topology:</div>
      <BulkEdit disabled={locked} onQueues={(capacity) => void networkCommand('set_all_queues', { capacity })}
        onLinks={(patch) => void networkCommand('set_all_links', patch)} />
    </Panel>
  );
}

function BulkEdit({ disabled, onQueues, onLinks }: { disabled: boolean; onQueues: (c: number) => void; onLinks: (p: Record<string, number>) => void }) {
  const read = (id: string) => (document.getElementById(id) as HTMLInputElement | null)?.valueAsNumber;
  return (
    <div>
      <div className="grid3">
        <div className="field"><label>Queue</label><input id="bulk-queue" type="number" min={0} max={256} defaultValue={10} disabled={disabled} /></div>
        <div className="field"><label>Delay ms</label><input id="bulk-delay" type="number" min={0} defaultValue={10} disabled={disabled} /></div>
        <div className="field"><label>Mbit/s</label><input id="bulk-bw" type="number" min={0} step={0.1} defaultValue={10} disabled={disabled} /></div>
      </div>
      <div className="btn-row">
        <button className="btn sm" disabled={disabled} onClick={() => { const q = read('bulk-queue'); if (q !== undefined && Number.isFinite(q)) onQueues(q); }}>Set all queues</button>
        <button className="btn sm" disabled={disabled} onClick={() => {
          const d = read('bulk-delay'), b = read('bulk-bw');
          if (d !== undefined && b !== undefined && Number.isFinite(d) && Number.isFinite(b)) onLinks({ delay_ms: d, bandwidth_mbps: b });
        }}>Set all links</button>
      </div>
    </div>
  );
}
