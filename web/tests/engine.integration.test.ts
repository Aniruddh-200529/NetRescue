// @vitest-environment node
// Integration tests against the real WebAssembly build of the C engine, plus a cross-check that the
// WebAssembly build and the native build (build/netrescue_api.exe) return identical results.
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import type { EngineClient } from '../src/engine/client';
import { EngineError } from '../src/engine/client';
import type { EngineVersion, ExperimentResult, NetworkState, SimState, TraceRecord } from '../src/engine/types';
import { loadTestEngine } from './engineHarness';

let engine: EngineClient;
let raw: (request: string) => string;
beforeAll(async () => { ({ client: engine, raw } = await loadTestEngine()); });

describe('C engine (WebAssembly) API', () => {
  it('reports the WebAssembly target', async () => {
    const v = await engine.call<EngineVersion>('version');
    expect(v.target).toBe('webassembly');
    expect(v.api_version).toBe(1);
  });

  it('rejects invalid input with an error instead of guessing', async () => {
    await expect(engine.call('configure', { config: { loss_probability: 3 } })).rejects.toBeInstanceOf(EngineError);
    await expect(engine.call('nonexistent')).rejects.toThrow('unknown command');
  });

  it('generates every topology type and validates disconnection', async () => {
    for (const type of ['line', 'bus', 'star', 'ring', 'tree', 'mesh', 'hybrid', 'random']) {
      const n = await engine.call<NetworkState>('generate_topology', { type, nodes: 7, seed: 4 });
      expect(n.nodes).toHaveLength(7);
      expect(n.route.length).toBeGreaterThanOrEqual(2);
    }
    const n = await engine.call<NetworkState>('add_node', { type: 'server', x: 0.5, y: 0.5 });
    const id = n.nodes[n.nodes.length - 1].id;
    const unreachable = await engine.call<NetworkState>('set_endpoints', { source: n.source, destination: id });
    expect(unreachable.route).toHaveLength(0);
    expect(unreachable.issues.some((i) => i.severity === 'error' && /unreachable/.test(i.message))).toBe(true);
    await expect(engine.call('sim_start')).rejects.toThrow(/unreachable/);
  });

  it('runs a live transfer whose metrics agree with its event trace', async () => {
    await engine.call('generate_topology', { type: 'line', nodes: 4 });
    await engine.call('set_all_links', { delay_ms: 10, bandwidth_mbps: 10 });
    let sim = await engine.call<SimState>('sim_start', {
      config: { protocol: 'go_back_n', packet_count: 16, window_size: 4, payload_bytes: 1000, packets_per_second: 1000, timeout_s: 0.15, max_retries: 15, loss_probability: 0.15, ack_loss_probability: 0.1, seed: 99 },
      scenario: [],
    });
    const trace: TraceRecord[] = [...(sim.trace ?? [])];
    while (sim.state === 'Running') {
      sim = await engine.call<SimState>('sim_step', { dt: 0.05, trace_from: trace.length, trace_limit: 5000 });
      trace.push(...(sim.trace ?? []));
    }
    expect(sim.state).toBe('Complete');
    const m = sim.metrics!;
    const count = (type: string) => trace.filter((r) => r.type === type).length;
    expect(trace).toHaveLength(sim.trace_total!);
    expect(count('DATA_SEND')).toBe(m.first_transmissions);
    expect(count('DATA_RETX')).toBe(m.retransmissions);
    expect(count('DATA_DELIVERED')).toBe(m.packets_delivered);
    expect(count('TIMEOUT')).toBe(m.timeouts);
    const ledger = sim.faults ?? [];
    expect(ledger).toHaveLength(m.faults_total);
    expect(count('DATA_LOST')).toBe(ledger.filter((f) => f.frame_kind === 'data' && f.type !== 'data_corruption').length);
    expect(count('ACK_LOST')).toBe(ledger.filter((f) => f.frame_kind !== 'data' && f.type !== 'ack_corruption').length);
    expect(ledger.every((f) => f.recovered)).toBe(true);
    expect(m.packets_delivered).toBe(16);
    expect(m.first_transmissions).toBe(16);
    expect(m.faults.data_loss.injected).toBeGreaterThan(0);
    expect(new Set(trace.filter((r) => r.type === 'DATA_DELIVERED').map((r) => r.seq)).size).toBe(16);
  });

  it('shows Go-Back-N versus Selective Repeat recovery in the captured trace', async () => {
    const r = await engine.call<ExperimentResult>('run_preset', { id: 'gbn_vs_sr' });
    const gbn = r.runs.find((x) => x.protocol === 'go_back_n')!, sr = r.runs.find((x) => x.protocol === 'selective_repeat')!;
    expect(gbn.retransmitted.map((x) => x.seq)).toEqual([4, 5, 6, 7, 8, 9, 10, 11]);
    expect(sr.retransmitted.map((x) => x.seq)).toEqual([4]);
    const gbnTrace = r.captures.find((c) => c.protocol === 'go_back_n')!.trace;
    expect(gbnTrace.filter((t) => t.type === 'DATA_LOST').map((t) => t.seq)).toEqual([4]);
    expect(gbnTrace.filter((t) => t.type === 'DATA_DISCARDED_OUT_OF_ORDER').map((t) => t.seq)).toEqual([5, 6, 7, 8, 9, 10, 11]);
    const srTrace = r.captures.find((c) => c.protocol === 'selective_repeat')!.trace;
    expect(srTrace.filter((t) => t.type === 'DATA_BUFFERED').map((t) => t.seq)).toEqual([5, 6, 7, 8, 9, 10, 11]);
  });

  it('demonstrates ACK loss after the data already arrived', async () => {
    const r = await engine.call<ExperimentResult>('run_preset', { id: 'ack_loss' });
    const cap = r.captures.find((c) => c.protocol === 'stop_and_wait' && c.sweep_index === 0)!;
    const accepted = cap.trace.find((t) => t.type === 'DATA_ACCEPTED' && t.seq === 3)!;
    const lost = cap.trace.find((t) => t.type === 'ACK_LOST' && t.seq === 3)!;
    const duplicate = cap.trace.find((t) => t.type === 'DATA_DUPLICATE' && t.seq === 3)!;
    expect(accepted.t).toBeLessThan(lost.t);
    expect(lost.t).toBeLessThan(duplicate.t);
    expect(cap.trace.filter((t) => t.type === 'DATA_DELIVERED' && t.seq === 3)).toHaveLength(1);
  });

  it('is reproducible: identical requests give identical runs', async () => {
    const request = { topology: { kind: 'preset', id: 'loss_sweep' }, trials: 3 };
    const a = await engine.call<ExperimentResult>('run_experiment', request);
    const b = await engine.call<ExperimentResult>('run_experiment', request);
    expect(JSON.stringify(a.runs)).toEqual(JSON.stringify(b.runs));
    expect(a.aggregates).toHaveLength(15);
  });

  it('exports experiment CSV with one row per run', async () => {
    const r = await engine.call<ExperimentResult>('run_preset', { id: 'ideal' });
    const { csv } = await engine.call<{ csv: string }>('export_experiment_csv');
    const lines = csv.trim().split('\n');
    expect(lines[0].startsWith('sweep_parameter,sweep_value,protocol')).toBe(true);
    expect(lines).toHaveLength(r.runs.length + 1);
    const header = lines[0].split(',');
    const goodput = header.indexOf('goodput_bps');
    for (let i = 1; i < lines.length; i++) expect(Number(lines[i].split(',')[goodput])).toBeCloseTo(r.runs[i - 1].metrics.goodput_bps!, 3);
  });
});


const nativeExe = resolve(process.cwd(), '../build/netrescue_api.exe');

describe.runIf(existsSync(nativeExe))('WebAssembly build matches the native C build', () => {
  it('returns byte-identical experiment runs and aggregates', () => {
    const requests = [
      { cmd: 'run_preset', id: 'gbn_vs_sr' },
      { cmd: 'run_experiment', topology: { kind: 'preset', id: 'corruption' }, trials: 2 },
      { cmd: 'run_experiment', topology: { kind: 'generate', type: 'random', nodes: 9, seed: 5 }, config: { packet_count: 30, loss_probability: 0.1, ack_corruption_probability: 0.05, window_size: 5 }, trials: 2, capture_trace: false },
    ];
    const input = requests.map((r) => JSON.stringify(r)).join('\n') + '\n';
    const nativeLines = execFileSync(nativeExe, [], { input, maxBuffer: 256 * 1024 * 1024 }).toString().trim().split(/\r?\n/);
    requests.forEach((request, i) => {
      const native = JSON.parse(nativeLines[i]).result as ExperimentResult;
      const wasm = JSON.parse(raw(JSON.stringify(request))).result as ExperimentResult;
      expect(JSON.stringify(wasm.runs)).toEqual(JSON.stringify(native.runs));
      expect(JSON.stringify(wasm.aggregates)).toEqual(JSON.stringify(native.aggregates));
      expect(wasm.findings).toEqual(native.findings);
    });
  });
});
