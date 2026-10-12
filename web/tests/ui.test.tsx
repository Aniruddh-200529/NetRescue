// Browser-control and rendering tests. Components talk to the real C engine (WebAssembly) through the
// same EngineClient the website uses; no engine behaviour is mocked.
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { beforeAll, beforeEach, describe, expect, it } from 'vitest';
import type { EngineClient } from '../src/engine/client';
import type { ArqConfig, ExperimentResult, SimState } from '../src/engine/types';
import { App } from '../src/App';
import { useLab } from '../src/state/labStore';
import { Results } from '../src/pages/compare/Results';
import { toRequest, type ExperimentForm } from '../src/pages/compare/ExperimentForm';
import { ScenarioEditor } from '../src/components/ScenarioEditor';
import { ProtocolSettings, RunControls } from '../src/pages/lab/SimControls';
import { loadTestEngine } from './engineHarness';

let engine: EngineClient;
beforeAll(async () => { ({ client: engine } = await loadTestEngine()); });

describe('App shell', () => {
  it('loads the C engine and offers four separate modes', async () => {
    render(<App engine={engine} />);
    await screen.findByText(/C engine · webassembly · API v1/);
    const nav = screen.getByRole('navigation', { name: 'Modes' });
    for (const label of ['Network Simulation', 'Protocol Comparison', 'Experiment History', 'Help / About']) expect(within(nav).getByText(label)).toBeInTheDocument();
    fireEvent.click(within(nav).getByText('Protocol Comparison'));
    expect(await screen.findByText('Protocol Comparison Laboratory')).toBeInTheDocument();
    expect(screen.getAllByText('Run').length).toBe(7); // the seven prebuilt experiments come from the engine
    fireEvent.click(within(nav).getByText('Help / About'));
    expect(await screen.findByText('What runs where')).toBeInTheDocument();
    expect(screen.getByText('Protocol efficiency')).toBeInTheDocument(); // metric definitions served by the engine
  });
});

describe('Simulation controls drive the engine', () => {
  beforeEach(async () => {
    await act(async () => { await useLab.getState().init(engine); });
    await act(async () => { await useLab.getState().networkCommand('generate_topology', { type: 'line', nodes: 4 }); });
  });

  it('sends protocol settings to the engine and starts a transfer', async () => {
    render(<><RunControls /><ProtocolSettings /></>);
    fireEvent.change(screen.getByLabelText('ARQ protocol'), { target: { value: 'selective_repeat' } });
    fireEvent.change(screen.getByLabelText('Packets'), { target: { value: '7' } });
    await waitFor(async () => {
      const { config } = await engine.call<{ config: ArqConfig }>('get_config');
      expect(config.protocol).toBe('selective_repeat');
      expect(config.packet_count).toBe(7);
    });
    fireEvent.click(screen.getByText('▶ Start'));
    await waitFor(() => expect(useLab.getState().sim?.state).toBe('Running'));
    expect(useLab.getState().sim?.protocol).toBe('selective_repeat');
    fireEvent.click(screen.getByText('❚❚ Pause'));
    await waitFor(() => expect(useLab.getState().sim?.state).toBe('Paused'));
    const before = useLab.getState().trace.length;
    fireEvent.click(screen.getByText('⏭ Step'));
    await waitFor(() => expect(useLab.getState().trace.length).toBeGreaterThan(before));
    fireEvent.click(screen.getByText('■ Stop'));
    await waitFor(() => expect(useLab.getState().sim?.state).toBe('Stopped'));
    // What the UI holds is exactly what the engine reports.
    const engineState = await engine.call<SimState>('sim_state', { trace_from: 0, trace_limit: 5000 });
    expect(useLab.getState().sim?.metrics).toEqual(engineState.metrics);
    expect(useLab.getState().trace.map((t) => t.i)).toEqual(engineState.trace!.map((t) => t.i));
  });

  it('surfaces engine validation errors', async () => {
    useLab.getState().setConfig({ timeout_s: 120 });
    await waitFor(() => expect(useLab.getState().message?.text).toMatch(/timeout_s/));
  });
});

describe('Fault rule editor', () => {
  it('builds deterministic rules', () => {
    let rules: unknown[] = [];
    const { rerender } = render(<ScenarioEditor rules={[]} onChange={(r) => { rules = r; }} links={[]} nodes={[]} />);
    fireEvent.click(screen.getByText('+ Add fault rule'));
    expect(rules).toEqual([{ type: 'drop_data', sequence: 3, attempt: 1 }]);
    rerender(<ScenarioEditor rules={rules as never} onChange={(r) => { rules = r; }} links={[]} nodes={[]} />);
    expect(screen.getByText('Drop DATA seq 3 (attempt 1)')).toBeInTheDocument();
  });
});

describe('Comparison results rendering', () => {
  it('maps the form to an engine request', () => {
    const form = {
      topology: { kind: 'generate', type: 'tree', nodes: 7, seed: 2, presetId: 'ideal' }, source: null, destination: null,
      config: useLab.getState().config, scenario: [{ type: 'drop_ack', sequence: 2, attempt: 1 }], protocols: ['go_back_n'],
      trials: 3, seed_base: 9, seed_stride: 2, sweepParameter: 'loss_probability', sweepValues: '0, 0.1 0.2',
      overrides: { link_delay_ms: '5', link_bandwidth_mbps: '', link_jitter_ms: 'x', queue_capacity: '' }, capture_trace: false,
    } as ExperimentForm;
    const request = toRequest(form);
    expect(request.topology).toEqual({ kind: 'generate', type: 'tree', nodes: 7, seed: 2 });
    expect(request.sweep).toEqual({ parameter: 'loss_probability', values: [0, 0.1, 0.2] });
    expect(request.overrides).toEqual({ link_delay_ms: 5 });
  });

  it('renders engine findings, aggregate table, evidence and trace for a real experiment', async () => {
    const result = await engine.call<ExperimentResult>('run_preset', { id: 'gbn_vs_sr' });
    const { csv } = await engine.call<{ csv: string }>('export_experiment_csv');
    render(<Results result={result} csv={csv} definitions={useLab.getState().definitions} />);
    expect(screen.getByText('Single run per protocol (no statistics)')).toBeInTheDocument();
    for (const finding of result.findings) expect(screen.getByText(finding)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Retransmission evidence' }));
    expect(screen.getByText('4, 5, 6, 7, 8, 9, 10, 11')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Results table' }));
    expect(screen.getAllByText('1/1')).toHaveLength(2);
    fireEvent.click(screen.getByRole('tab', { name: 'Captured traces' }));
    expect(screen.getAllByText(/DATA seq 4 \(attempt 1\) lost in transit/).length).toBeGreaterThan(0);
  });

  it('shows mean ± standard deviation for repeated trials', async () => {
    const result = await engine.call<ExperimentResult>('run_experiment', { topology: { kind: 'preset', id: 'loss_sweep' }, trials: 4, sweep: { parameter: 'loss_probability', values: [0.2] } });
    render(<Results result={result} csv="" definitions={[]} />);
    expect(screen.getByText(/Aggregate of 4 independent trials/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Results table' }));
    const gbn = result.aggregates.find((a) => a.protocol === 'go_back_n')!;
    const s = gbn.stats.retransmissions;
    expect(screen.getByText(`${s.mean!.toLocaleString(undefined, { maximumFractionDigits: 1 })} ± ${s.stddev!.toLocaleString(undefined, { maximumFractionDigits: 1 })}`)).toBeInTheDocument();
  });
});
