import { useEffect, useState } from 'react';
import type { EngineClient } from '../../engine/client';
import type { ExperimentResult, ExperimentSpec, ProtocolId } from '../../engine/types';
import { useLab, DEFAULT_CONFIG } from '../../state/labStore';
import { experimentTitle, saveHistory, takePendingEntry } from '../../lib/history';
import { ExperimentFormView, toRequest, type ExperimentForm } from './ExperimentForm';
import { Results } from './Results';

const DEFAULT_FORM: ExperimentForm = {
  topology: { kind: 'generate', type: 'ring', nodes: 8, seed: 1, presetId: 'ideal' },
  source: null, destination: null,
  config: { ...DEFAULT_CONFIG, packet_count: 100, window_size: 8, payload_bytes: 1000, timeout_s: 0.2, packets_per_second: 500 },
  scenario: [], protocols: ['stop_and_wait', 'go_back_n', 'selective_repeat'],
  trials: 5, seed_base: 1000, seed_stride: 7919, sweepParameter: 'none', sweepValues: '',
  overrides: { link_delay_ms: '', link_bandwidth_mbps: '', link_jitter_ms: '', queue_capacity: '' }, capture_trace: true,
};

function formFromSpec(spec: ExperimentSpec, presetId: string): ExperimentForm {
  const o = spec.overrides;
  return {
    topology: { ...DEFAULT_FORM.topology, kind: 'preset', presetId }, source: null, destination: null,
    config: spec.config, scenario: spec.scenario, protocols: spec.protocols as ProtocolId[], trials: spec.trials,
    seed_base: spec.seed_base, seed_stride: spec.seed_stride, sweepParameter: spec.sweep.parameter, sweepValues: spec.sweep.values.join(', '),
    overrides: { link_delay_ms: o.link_delay_ms?.toString() ?? '', link_bandwidth_mbps: o.link_bandwidth_mbps?.toString() ?? '', link_jitter_ms: o.link_jitter_ms?.toString() ?? '', queue_capacity: o.queue_capacity?.toString() ?? '' },
    capture_trace: spec.capture_trace,
  };
}

export function ComparisonLab({ engine }: { engine: EngineClient }) {
  const { presets, definitions, network, notify } = useLab();
  const [form, setForm] = useState<ExperimentForm>(DEFAULT_FORM);
  const [running, setRunning] = useState<string | null>(null);
  const [result, setResult] = useState<{ result: ExperimentResult; csv: string } | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const entry = takePendingEntry();
    if (entry) setResult({ result: entry.result, csv: entry.csv });
  }, []);

  const finish = async (r: ExperimentResult) => {
    const { csv } = await engine.call<{ csv: string }>('export_experiment_csv');
    setResult({ result: r, csv });
    try {
      await saveHistory({ id: crypto.randomUUID(), savedAt: new Date().toISOString(), title: experimentTitle(r), result: r, csv });
    } catch { notify('info', 'Result shown but could not be saved to history (browser storage unavailable)'); }
  };

  const run = async (label: string, work: () => Promise<ExperimentResult>) => {
    setRunning(label); setError(null);
    try { await finish(await work()); } catch (e) { setError((e as Error).message); } finally { setRunning(null); }
  };

  const runPreset = (id: string, title: string) => run(title, () => engine.call<ExperimentResult>('run_preset', { id }));
  const loadPreset = async (id: string) => {
    try {
      const { spec } = await engine.call<{ spec: ExperimentSpec }>('preset_spec', { id });
      setForm(formFromSpec(spec, id));
      notify('info', 'Preset loaded into the form: adjust any setting and press Run comparison');
    } catch (e) { notify('error', (e as Error).message); }
  };

  return (
    <div className="mode-b">
      <div className="mode-banner">
        <svg width="34" height="34" viewBox="0 0 34 34" aria-hidden><rect x="4" y="16" width="6" height="14" rx="2" fill="#3987e5" /><rect x="14" y="8" width="6" height="22" rx="2" fill="#d95926" /><rect x="24" y="4" width="6" height="26" rx="2" fill="#199e70" /></svg>
        <div>
          <h2>Protocol Comparison Laboratory</h2>
          <div className="small secondary">Runs Stop-and-Wait, Go-Back-N and Selective Repeat independently in the C engine under identical topology, faults and seeds, with statistics over repeated trials.</div>
        </div>
      </div>

      <section className="panel" style={{ marginBottom: 14 }}>
        <div className="panel-head"><h3>Prebuilt experiments (one click, executed by the C engine)</h3></div>
        <div className="panel-body">
          <div className="preset-grid">
            {presets.map((p) => (
              <div className="preset" key={p.id}>
                <h4>{p.title}</h4>
                <p>{p.purpose}</p>
                <details><summary className="small muted" style={{ cursor: 'pointer' }}>Settings & what to look for</summary>
                  <p style={{ marginTop: 6 }}><b>Settings:</b> {p.settings}</p><p style={{ marginTop: 6 }}><b>Look for:</b> {p.what_to_look_for}</p></details>
                <div className="btn-row" style={{ marginTop: 'auto' }}>
                  <button className="btn sm violet" disabled={!!running} onClick={() => void runPreset(p.id, p.title)}>Run</button>
                  <button className="btn sm" disabled={!!running} onClick={() => void loadPreset(p.id)}>Load into form</button>
                </div>
              </div>
            ))}
          </div>
        </div>
      </section>

      <div className="compare">
        <div>
          <ExperimentFormView form={form} onChange={setForm} nodes={network?.nodes ?? []} links={network?.links ?? []} presetIds={presets.map((p) => ({ id: p.id, title: p.title }))} />
          <div className="btn-row" style={{ marginTop: 12 }}>
            <button className="btn violet" disabled={!!running || form.protocols.length === 0} onClick={() => void run('Custom comparison', () => engine.call<ExperimentResult>('run_experiment', toRequest(form)))}>▶ Run comparison</button>
            <button className="btn ghost" disabled={!!running} onClick={() => setForm(DEFAULT_FORM)}>Reset form</button>
          </div>
        </div>
        <div>
          {running && (
            <section className="panel panel-body" style={{ marginBottom: 12 }}>
              <div style={{ marginBottom: 8 }}>Running “{running}” in the C engine (Web Worker)…</div>
              <div className="progress"><div /></div>
            </section>
          )}
          {error && <section className="panel panel-body" style={{ borderColor: 'var(--critical)', marginBottom: 12 }}>The engine rejected the experiment: {error}</section>}
          {result ? <Results result={result.result} csv={result.csv} definitions={definitions} /> : !running && (
            <section className="panel"><div className="empty">Run a prebuilt experiment or configure your own comparison. Results, charts and exports appear here.</div></section>
          )}
        </div>
      </div>
    </div>
  );
}
