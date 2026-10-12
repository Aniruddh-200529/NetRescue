// Application state for the Network Simulation Lab. It only stores what the C engine returns and
// forwards user actions as engine commands; it never computes packet outcomes or metrics itself.
import { create } from 'zustand';
import type { EngineClient } from '../engine/client';
import type {
  ArqConfig, EngineVersion, FaultRecord, FaultRule, HistorySample, MetricDefinition, NetworkState, PresetInfo, SimState, TraceRecord,
} from '../engine/types';

export type Selection = { kind: 'node'; id: number } | { kind: 'link'; id: number } | null;
export interface VisualEffect { key: string; kind: 'loss' | 'corrupt' | 'queue' | 'outage' | 'timeout'; node: number; link: number; label: string; until: number }

const TRACE_KEEP = 20000;

export const DEFAULT_CONFIG: ArqConfig = {
  protocol: 'go_back_n', packet_count: 20, window_size: 4, payload_bytes: 512, packets_per_second: 100, timeout_s: 0.25,
  loss_probability: 0, corruption_probability: 0, ack_loss_probability: 0, ack_corruption_probability: 0, seed: 424242,
  max_retries: 15, max_sim_time: 0, congestion_mode: 'none', use_nak: true, corruption_bits: 1, cross_traffic_pps: 0, cross_traffic_bytes: 1000,
};

interface LabState {
  engine: EngineClient | null;
  version: EngineVersion | null;
  definitions: MetricDefinition[];
  presets: PresetInfo[];
  engineError: string | null;
  network: NetworkState | null;
  config: ArqConfig;
  scenario: FaultRule[];
  sim: SimState | null;
  trace: TraceRecord[];
  traceOffset: number;
  faults: FaultRecord[];
  history: HistorySample[];
  faultVersion: number;
  historyVersion: number;
  speed: number;
  busy: boolean;
  selection: Selection;
  linkMode: number | null;
  message: { kind: 'error' | 'info'; text: string } | null;
  effects: VisualEffect[];

  init(engine: EngineClient): Promise<void>;
  networkCommand(cmd: string, params?: Record<string, unknown>): Promise<boolean>;
  setConfig(patch: Partial<ArqConfig>): void;
  setScenario(rules: FaultRule[]): void;
  start(): Promise<void>;
  simCommand(cmd: 'sim_pause' | 'sim_resume' | 'sim_stop' | 'sim_reset' | 'sim_step_event'): Promise<void>;
  step(dt: number): Promise<void>;
  setSpeed(speed: number): void;
  select(selection: Selection): void;
  setLinkMode(from: number | null): void;
  notify(kind: 'error' | 'info', text: string): void;
  pruneEffects(now: number): void;
}

function effectsFrom(records: TraceRecord[], now: number): VisualEffect[] {
  const effects: VisualEffect[] = [];
  for (const r of records) {
    const base = { key: `${r.i}`, node: r.node, link: r.link, until: now + 1400 };
    if (r.type === 'DATA_LOST' || r.type === 'ACK_LOST') {
      const kind = r.fault_type === 'queue_overflow' ? 'queue' : r.fault_type === 'link_outage' ? 'outage' : 'loss';
      effects.push({ ...base, kind, label: `${r.type === 'DATA_LOST' ? 'DATA' : 'ACK'} ${r.seq} ${kind === 'queue' ? 'dropped' : 'lost'}` });
    } else if (r.type === 'CORRUPTION_INJECTED') effects.push({ ...base, kind: 'corrupt', label: `bits flipped (seq ${r.seq})` });
    else if (r.type === 'DATA_CORRUPT_DETECTED' || r.type === 'ACK_CORRUPT_DETECTED') effects.push({ ...base, kind: 'corrupt', label: `checksum fail seq ${r.seq}` });
    else if (r.type === 'TIMEOUT') effects.push({ ...base, kind: 'timeout', label: `timeout seq ${r.seq}` });
  }
  return effects;
}

export const useLab = create<LabState>((set, get) => {
  const applySim = (sim: SimState, fresh = false) => {
    const state = get();
    const now = performance.now();
    let trace = fresh ? [] : state.trace;
    let traceOffset = fresh ? 0 : state.traceOffset;
    let newRecords: TraceRecord[] = [];
    if (sim.trace) {
      newRecords = sim.trace.filter((r) => r.i >= traceOffset + trace.length);
      trace = trace.concat(newRecords);
      if (trace.length > TRACE_KEEP) { traceOffset += trace.length - TRACE_KEEP; trace = trace.slice(trace.length - TRACE_KEEP); }
    }
    set({
      sim,
      trace,
      traceOffset,
      faults: sim.faults ?? (fresh ? [] : state.faults),
      faultVersion: sim.fault_version ?? -1,
      history: sim.history ?? (fresh ? [] : state.history),
      historyVersion: sim.history_version ?? -1,
      effects: state.effects.filter((e) => e.until > now).concat(effectsFrom(newRecords, now)).slice(-40),
    });
  };
  const cursor = () => {
    const s = get();
    return { trace_from: s.traceOffset + s.trace.length, trace_limit: 3000, fault_version: s.faultVersion, history_version: s.historyVersion };
  };
  const guard = async <T,>(work: () => Promise<T>): Promise<T | undefined> => {
    try { return await work(); } catch (error) { get().notify('error', (error as Error).message); return undefined; }
  };

  return {
    engine: null, version: null, definitions: [], presets: [], engineError: null, network: null,
    config: DEFAULT_CONFIG, scenario: [], sim: null, trace: [], traceOffset: 0, faults: [], history: [],
    faultVersion: -1, historyVersion: -1, speed: 0.1, busy: false, selection: null, linkMode: null, message: null, effects: [],

    async init(engine) {
      try {
        const version = await engine.call<EngineVersion>('version');
        const [definitions, presets, network, configured] = await Promise.all([
          engine.call<MetricDefinition[]>('metric_definitions'),
          engine.call<PresetInfo[]>('list_presets'),
          engine.call<NetworkState>('get_network'),
          engine.call<{ config: ArqConfig; scenario: FaultRule[] }>('configure', { config: DEFAULT_CONFIG }),
        ]);
        set({ engine, version, definitions, presets, network, config: configured.config, scenario: configured.scenario, engineError: null });
      } catch (error) {
        set({ engineError: (error as Error).message });
      }
    },

    async networkCommand(cmd, params = {}) {
      const { engine } = get();
      if (!engine) return false;
      const network = await guard(() => engine.call<NetworkState>(cmd, params));
      if (!network) return false;
      set({ network });
      const sel = get().selection;
      if (sel?.kind === 'node' && !network.nodes.some((n) => n.id === sel.id)) set({ selection: null });
      if (sel?.kind === 'link' && !network.links.some((l) => l.id === sel.id)) set({ selection: null });
      // A finished simulation is released by the engine when the topology structure changes.
      if (get().sim && !get().sim?.active && ['generate_topology', 'load_demo', 'add_node', 'remove_node', 'add_link', 'remove_link', 'import_topology', 'set_endpoints'].includes(cmd)) {
        set({ sim: null, trace: [], traceOffset: 0, faults: [], history: [] });
      }
      return true;
    },

    setConfig(patch) {
      const config = { ...get().config, ...patch };
      set({ config });
      void guard(async () => {
        const engine = get().engine;
        if (!engine) return;
        const result = await engine.call<{ config: ArqConfig }>('configure', { config: patch });
        set({ config: { ...get().config, ...result.config } });
      });
    },

    setScenario(rules) {
      set({ scenario: rules });
      void guard(async () => { await get().engine?.call('configure', { scenario: rules }); });
    },

    async start() {
      const { engine, config, scenario } = get();
      if (!engine) return;
      set({ busy: true });
      const sim = await guard(() => engine.call<SimState>('sim_start', { config, scenario, trace_from: 0 }));
      set({ busy: false });
      if (sim) {
        set({ trace: [], traceOffset: 0, faults: [], history: [], faultVersion: -1, historyVersion: -1, effects: [] });
        applySim(sim, true);
        const network = await guard(() => engine.call<NetworkState>('get_network'));
        if (network) set({ network });
      }
    },

    async simCommand(cmd) {
      const { engine } = get();
      if (!engine) return;
      const sim = await guard(() => engine.call<SimState>(cmd, cursor()));
      if (!sim) return;
      if (cmd === 'sim_reset') {
        set({ sim: null, trace: [], traceOffset: 0, faults: [], history: [], faultVersion: -1, historyVersion: -1, effects: [] });
        const network = await guard(() => engine.call<NetworkState>('get_network'));
        if (network) set({ network });
        return;
      }
      applySim(sim);
      if (cmd === 'sim_stop') {
        const network = await guard(() => engine.call<NetworkState>('get_network'));
        if (network) set({ network });
      }
    },

    async step(dt) {
      const { engine, busy } = get();
      if (!engine || busy) return;
      set({ busy: true });
      const sim = await guard(() => engine.call<SimState>('sim_step', { dt, ...cursor() }));
      set({ busy: false });
      if (sim) {
        const wasActive = get().sim?.active;
        applySim(sim);
        if (wasActive && !sim.active) {
          const network = await guard(() => engine.call<NetworkState>('get_network'));
          if (network) set({ network });
        }
      }
    },

    setSpeed(speed) { set({ speed }); },
    select(selection) { set({ selection }); },
    setLinkMode(from) { set({ linkMode: from }); },
    notify(kind, text) { set({ message: { kind, text } }); },
    pruneEffects(now) { const effects = get().effects; if (effects.some((e) => e.until <= now)) set({ effects: effects.filter((e) => e.until > now) }); },
  };
});
