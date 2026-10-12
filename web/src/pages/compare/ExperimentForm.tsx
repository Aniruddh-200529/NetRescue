import type { ArqConfig, FaultRule, NetLink, NetNode, ProtocolId, TopologyType } from '../../engine/types';
import { NumberField, Panel, ProbabilityField, SelectField } from '../../components/Fields';
import { ScenarioEditor } from '../../components/ScenarioEditor';
import { PROTOCOL_COLOR, PROTOCOL_LABEL, PROTOCOLS } from '../../lib/format';

export interface ExperimentForm {
  topology: { kind: 'current' | 'generate' | 'preset'; type: TopologyType; nodes: number; seed: number; presetId: string };
  source: number | null; destination: number | null;
  config: ArqConfig; scenario: FaultRule[]; protocols: ProtocolId[];
  trials: number; seed_base: number; seed_stride: number;
  sweepParameter: string; sweepValues: string;
  overrides: { link_delay_ms: string; link_bandwidth_mbps: string; link_jitter_ms: string; queue_capacity: string };
  capture_trace: boolean;
}

export const SWEEP_PARAMETERS: { value: string; label: string; example: string }[] = [
  { value: 'none', label: 'No sweep (one configuration)', example: '' },
  { value: 'loss_probability', label: 'Data loss probability', example: '0, 0.05, 0.1, 0.2, 0.3' },
  { value: 'corruption_probability', label: 'Data corruption probability', example: '0, 0.05, 0.1, 0.2' },
  { value: 'ack_loss_probability', label: 'ACK loss probability', example: '0, 0.1, 0.2, 0.3' },
  { value: 'ack_corruption_probability', label: 'ACK corruption probability', example: '0, 0.1, 0.2' },
  { value: 'timeout_s', label: 'Timeout (seconds)', example: '0.05, 0.1, 0.2, 0.5' },
  { value: 'window_size', label: 'Window size (GBN / SR)', example: '1, 2, 4, 8, 16' },
  { value: 'packets_per_second', label: 'Offered rate (pkt/s)', example: '50, 100, 500, 1000' },
  { value: 'queue_capacity', label: 'Queue capacity (all nodes)', example: '1, 2, 5, 10' },
  { value: 'link_delay_ms', label: 'Link delay (all links, ms)', example: '1, 5, 20, 50' },
  { value: 'link_bandwidth_mbps', label: 'Link bandwidth (all links, Mbit/s)', example: '0.5, 1, 10, 100' },
  { value: 'link_jitter_ms', label: 'Link jitter (all links, ms)', example: '0, 2, 5, 10' },
  { value: 'congestion_mode', label: 'Congestion control (0 none, 1 rate, 2 window)', example: '0, 1, 2' },
  { value: 'payload_bytes', label: 'Payload size (bytes)', example: '100, 500, 1000, 1500' },
];

export function toRequest(form: ExperimentForm): Record<string, unknown> {
  const topology = form.topology.kind === 'current' ? { kind: 'current' }
    : form.topology.kind === 'preset' ? { kind: 'preset', id: form.topology.presetId }
      : { kind: 'generate', type: form.topology.type, nodes: form.topology.nodes, seed: form.topology.seed };
  const overrides: Record<string, number> = {};
  for (const [k, v] of Object.entries(form.overrides)) if (v.trim() !== '' && Number.isFinite(Number(v))) overrides[k] = Number(v);
  const values = form.sweepValues.split(/[,\s]+/).filter(Boolean).map(Number);
  const request: Record<string, unknown> = {
    topology, config: form.config, scenario: form.scenario, protocols: form.protocols, trials: form.trials,
    seed_base: form.seed_base, seed_stride: form.seed_stride, overrides, capture_trace: form.capture_trace,
    sweep: { parameter: form.sweepParameter, values: form.sweepParameter === 'none' ? [] : values },
  };
  if (form.source !== null) request.source = form.source;
  if (form.destination !== null) request.destination = form.destination;
  return request;
}

export function ExperimentFormView({ form, onChange, nodes, links, presetIds }: {
  form: ExperimentForm; onChange: (form: ExperimentForm) => void; nodes: NetNode[]; links: NetLink[]; presetIds: { id: string; title: string }[];
}) {
  const set = (patch: Partial<ExperimentForm>) => onChange({ ...form, ...patch });
  const cfg = (patch: Partial<ArqConfig>) => set({ config: { ...form.config, ...patch } });
  const sweep = SWEEP_PARAMETERS.find((p) => p.value === form.sweepParameter);
  const toggle = (p: ProtocolId) => set({ protocols: form.protocols.includes(p) ? form.protocols.filter((x) => x !== p) : PROTOCOLS.filter((x) => x === p || form.protocols.includes(x)) });

  return (
    <div className="stack">
      <Panel title="1 · Topology & endpoints">
        <SelectField label="Topology" value={form.topology.kind} onChange={(kind) => set({ topology: { ...form.topology, kind }, source: null, destination: null })}
          options={[{ value: 'current', label: 'Current Network Simulation topology' }, { value: 'generate', label: 'Generate a topology' }, { value: 'preset', label: 'Preset experiment topology' }]} />
        {form.topology.kind === 'generate' && (
          <div className="grid3">
            <div className="field"><label>Type</label>
              <select value={form.topology.type} onChange={(e) => set({ topology: { ...form.topology, type: e.target.value as TopologyType } })}>
                {['line', 'bus', 'star', 'ring', 'tree', 'mesh', 'hybrid', 'random'].map((t) => <option key={t} value={t}>{t}</option>)}
              </select></div>
            <NumberField label="Nodes" value={form.topology.nodes} min={2} max={64} onChange={(nodes) => set({ topology: { ...form.topology, nodes } })} />
            <NumberField label="Seed" value={form.topology.seed} min={0} onChange={(seed) => set({ topology: { ...form.topology, seed } })} />
          </div>
        )}
        {form.topology.kind === 'preset' && (
          <SelectField label="Preset topology" value={form.topology.presetId} onChange={(presetId) => set({ topology: { ...form.topology, presetId } })}
            options={presetIds.map((p) => ({ value: p.id, label: p.title }))} />
        )}
        {form.topology.kind === 'current' ? (
          <div className="grid2">
            <div className="field"><label>Source</label>
              <select value={form.source ?? ''} onChange={(e) => set({ source: e.target.value === '' ? null : Number(e.target.value) })}>
                <option value="">Lab source</option>{nodes.map((n) => <option key={n.id} value={n.id}>{n.name}</option>)}
              </select></div>
            <div className="field"><label>Destination</label>
              <select value={form.destination ?? ''} onChange={(e) => set({ destination: e.target.value === '' ? null : Number(e.target.value) })}>
                <option value="">Lab destination</option>{nodes.map((n) => <option key={n.id} value={n.id}>{n.name}</option>)}
              </select></div>
          </div>
        ) : <p className="small muted" style={{ margin: 0 }}>Endpoints: the topology's first and last host (chosen by the engine).</p>}
        <div className="small muted" style={{ marginBottom: 6 }}>Optional overrides applied to every link / node of the experiment copy:</div>
        <div className="grid2">
          {([['link_delay_ms', 'Link delay (ms)'], ['link_bandwidth_mbps', 'Bandwidth (Mbit/s)'], ['link_jitter_ms', 'Jitter (ms)'], ['queue_capacity', 'Queue capacity']] as const).map(([k, label]) => (
            <div className="field" key={k}><label>{label}</label>
              <input type="text" inputMode="decimal" placeholder="keep" value={form.overrides[k]} onChange={(e) => set({ overrides: { ...form.overrides, [k]: e.target.value } })} /></div>
          ))}
        </div>
      </Panel>

      <Panel title="2 · Protocols">
        <div className="btn-row">
          {PROTOCOLS.map((p) => (
            <label key={p} className="tag" style={{ cursor: 'pointer', padding: '5px 10px', borderColor: form.protocols.includes(p) ? PROTOCOL_COLOR[p] : undefined }}>
              <input type="checkbox" checked={form.protocols.includes(p)} onChange={() => toggle(p)} aria-label={PROTOCOL_LABEL[p]} />
              <span className="swatch" style={{ background: PROTOCOL_COLOR[p] }} />{PROTOCOL_LABEL[p]}
            </label>
          ))}
        </div>
        <p className="small muted" style={{ marginBottom: 0 }}>Stop-and-Wait always uses window 1; Go-Back-N and Selective Repeat share the window below.</p>
      </Panel>

      <Panel title="3 · Transfer & link settings">
        <div className="grid2">
          <NumberField label="Packets" value={form.config.packet_count} min={1} max={1024} onChange={(packet_count) => cfg({ packet_count })} />
          <NumberField label="Payload" suffix="bytes" value={form.config.payload_bytes} min={1} max={65000} onChange={(payload_bytes) => cfg({ payload_bytes })} />
          <NumberField label="Window (GBN/SR)" value={form.config.window_size} min={1} max={1024} onChange={(window_size) => cfg({ window_size })} />
          <NumberField label="Timeout" suffix="ms" value={Math.round(form.config.timeout_s * 1000)} min={1} max={60000} onChange={(v) => cfg({ timeout_s: v / 1000 })} />
          <NumberField label="Max retries" value={form.config.max_retries} min={0} max={1000} onChange={(max_retries) => cfg({ max_retries })} />
          <NumberField label="Offered rate" suffix="pkt/s" value={form.config.packets_per_second} min={0.01} max={100000} onChange={(packets_per_second) => cfg({ packets_per_second })} />
          <NumberField label="Time limit" suffix="s (0 = 600)" value={form.config.max_sim_time} min={0} max={100000} onChange={(max_sim_time) => cfg({ max_sim_time })} />
          <SelectField label="Congestion control" value={form.config.congestion_mode} onChange={(congestion_mode) => cfg({ congestion_mode })}
            options={[{ value: 'none', label: 'None' }, { value: 'rate_aimd', label: 'Rate AIMD' }, { value: 'window_aimd', label: 'Window AIMD' }]} />
        </div>
      </Panel>

      <Panel title="4 · Fault model">
        <div className="grid2">
          <ProbabilityField label="Data loss" value={form.config.loss_probability} onChange={(loss_probability) => cfg({ loss_probability })} />
          <ProbabilityField label="Data corruption" value={form.config.corruption_probability} onChange={(corruption_probability) => cfg({ corruption_probability })} />
          <ProbabilityField label="ACK loss" value={form.config.ack_loss_probability} onChange={(ack_loss_probability) => cfg({ ack_loss_probability })} />
          <ProbabilityField label="ACK corruption" value={form.config.ack_corruption_probability} onChange={(ack_corruption_probability) => cfg({ ack_corruption_probability })} />
        </div>
        <label className="small" style={{ display: 'flex', gap: 6, marginBottom: 10 }}><input type="checkbox" checked={form.config.use_nak} onChange={(e) => cfg({ use_nak: e.target.checked })} /> NAK on detected corruption</label>
        <ScenarioEditor rules={form.scenario} onChange={(scenario) => set({ scenario })} links={form.topology.kind === 'current' ? links : []} nodes={form.topology.kind === 'current' ? nodes : []} />
      </Panel>

      <Panel title="5 · Trials, seeds & sweep">
        <div className="grid3">
          <NumberField label="Trials" value={form.trials} min={1} max={200} onChange={(trials) => set({ trials })} />
          <NumberField label="Seed base" value={form.seed_base} min={0} onChange={(seed_base) => set({ seed_base })} />
          <NumberField label="Seed stride" value={form.seed_stride} min={0} onChange={(seed_stride) => set({ seed_stride })} />
        </div>
        <p className="small muted" style={{ marginTop: 0 }}>Trial i uses seed = base + i × stride for every protocol and sweep value, so each protocol faces the same impairment draws.</p>
        <SelectField label="Sweep parameter" value={form.sweepParameter} onChange={(sweepParameter) => set({ sweepParameter, sweepValues: SWEEP_PARAMETERS.find((p) => p.value === sweepParameter)?.example ?? '' })}
          options={SWEEP_PARAMETERS.map((p) => ({ value: p.value, label: p.label }))} />
        {form.sweepParameter !== 'none' && (
          <div className="field"><label>Values (comma separated, up to 12)</label>
            <input type="text" value={form.sweepValues} placeholder={sweep?.example} onChange={(e) => set({ sweepValues: e.target.value })} /></div>
        )}
        <label className="small" style={{ display: 'flex', gap: 6 }}><input type="checkbox" checked={form.capture_trace} onChange={(e) => set({ capture_trace: e.target.checked })} /> Capture the full event trace of trial 0 (evidence view)</label>
      </Panel>
    </div>
  );
}
