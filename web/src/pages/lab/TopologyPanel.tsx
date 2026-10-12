import { useState } from 'react';
import type { NetworkState, NodeType, TopologyType } from '../../engine/types';
import { Panel } from '../../components/Fields';
import { useLab } from '../../state/labStore';
import { downloadText, readFileText, stamp } from '../../lib/download';

const TOPOLOGIES: TopologyType[] = ['line', 'bus', 'star', 'ring', 'tree', 'mesh', 'hybrid', 'random'];
const NODE_TYPES: { value: NodeType; label: string }[] = [
  { value: 'host', label: 'Host' }, { value: 'router', label: 'Router' }, { value: 'switch', label: 'Switch' },
  { value: 'server', label: 'Server' }, { value: 'access_point', label: 'Access point' },
];

export function TopologyPanel() {
  const { network, sim, engine, networkCommand, notify, selection, setLinkMode } = useLab();
  const [type, setType] = useState<TopologyType>('ring');
  const [count, setCount] = useState(8);
  const [seed, setSeed] = useState(1);
  const [nodeType, setNodeType] = useState<NodeType>('router');
  const locked = !!sim?.active;

  const save = async () => {
    if (!engine) return;
    try {
      const { text } = await engine.call<{ text: string }>('export_topology');
      downloadText(`netrescue-topology-${stamp()}.topo`, text);
    } catch (e) { notify('error', (e as Error).message); }
  };
  const saveJson = () => network && downloadText(`netrescue-topology-${stamp()}.json`, JSON.stringify(network, null, 2), 'application/json');
  const load = async () => {
    const text = await readFileText('.topo,.txt');
    if (text !== null && (await networkCommand('import_topology', { text }))) notify('info', 'Topology loaded and validated by the engine');
  };

  return (
    <Panel title="Topology">
      <div className="grid2">
        <div className="field"><label>Template</label>
          <select value={type} onChange={(e) => setType(e.target.value as TopologyType)} disabled={locked} aria-label="Topology template">
            {TOPOLOGIES.map((t) => <option key={t} value={t}>{t === 'mesh' ? 'Full mesh' : t === 'random' ? 'Seeded random' : t[0].toUpperCase() + t.slice(1)}</option>)}
          </select></div>
        <div className="field"><label>Nodes</label>
          <input type="number" min={2} max={type === 'mesh' ? 23 : 64} value={count} disabled={locked} onChange={(e) => setCount(Math.max(2, e.target.valueAsNumber || 2))} aria-label="Node count" /></div>
      </div>
      <div className="grid2">
        <div className="field"><label>Seed</label><input type="number" min={0} value={seed} disabled={locked} onChange={(e) => setSeed(Math.max(0, e.target.valueAsNumber || 0))} /></div>
        <div className="field"><label>&nbsp;</label><button className="btn primary" disabled={locked} onClick={() => void networkCommand('generate_topology', { type, nodes: count, seed })}>Generate</button></div>
      </div>
      <div className="btn-row" style={{ marginBottom: 10 }}>
        <button className="btn sm" disabled={locked} onClick={() => void networkCommand('load_demo')}>Demo network</button>
        <button className="btn sm" onClick={() => void save()}>Save .topo</button>
        <button className="btn sm" disabled={locked} onClick={() => void load()}>Load .topo</button>
        <button className="btn sm" onClick={saveJson}>Export JSON</button>
      </div>
      <div className="grid2">
        <div className="field"><label>Device</label>
          <select value={nodeType} disabled={locked} onChange={(e) => setNodeType(e.target.value as NodeType)}>
            {NODE_TYPES.map((t) => <option key={t.value} value={t.value}>{t.label}</option>)}
          </select></div>
        <div className="field"><label>&nbsp;</label>
          <button className="btn" disabled={locked} onClick={() => void networkCommand('add_node', { type: nodeType, x: 0.1 + Math.random() * 0.8, y: 0.1 + Math.random() * 0.8 })}>+ Add device</button></div>
      </div>
      <button className="btn sm" disabled={locked || selection?.kind !== 'node'} onClick={() => selection?.kind === 'node' && setLinkMode(selection.id)}
        title="Select a node, press this, then click the node to connect to">⟷ Connect selected node…</button>
      <div className="field" style={{ marginTop: 10 }}><label>Routing metric (Dijkstra)</label>
        <select value={network?.cost_mode ?? 'link'} disabled={locked} onChange={(e) => void networkCommand('set_cost_mode', { mode: e.target.value })}>
          <option value="link">Link cost</option><option value="delay">Delay</option><option value="composite">Composite (cost + delay + load)</option>
        </select></div>
    </Panel>
  );
}

export function EndpointsPanel() {
  const { network, sim, networkCommand } = useLab();
  if (!network) return null;
  const locked = !!sim?.active;
  const name = (id: number) => network.nodes.find((n) => n.id === id)?.name ?? '?';
  return (
    <Panel title="Transfer endpoints">
      <div className="grid2">
        <div className="field"><label>Source</label>
          <select value={network.source} disabled={locked} aria-label="Source" onChange={(e) => void networkCommand('set_endpoints', { source: Number(e.target.value), destination: network.destination })}>
            {network.nodes.map((n) => <option key={n.id} value={n.id}>{n.name}</option>)}
          </select></div>
        <div className="field"><label>Destination</label>
          <select value={network.destination} disabled={locked} aria-label="Destination" onChange={(e) => void networkCommand('set_endpoints', { source: network.source, destination: Number(e.target.value) })}>
            {network.nodes.map((n) => <option key={n.id} value={n.id}>{n.name}</option>)}
          </select></div>
      </div>
      <div className="small">
        <div><span className="muted">Dijkstra route: </span>{network.route.length ? network.route.map(name).join(' → ') : <span style={{ color: 'var(--critical)' }}>unreachable</span>}</div>
        <div><span className="muted">ACK route: </span>{network.reverse_route.length ? network.reverse_route.map(name).join(' → ') : 'unreachable'}</div>
        <div><span className="muted">No-load RTT: </span>{network.base_rtt_ms !== null ? `${network.base_rtt_ms.toFixed(2)} ms` : 'n/a'} <span className="muted">(engine-computed, current payload)</span></div>
      </div>
    </Panel>
  );
}

export function IssuesPanel({ network }: { network: NetworkState }) {
  const shown = network.issues.filter((i) => i.severity !== 'info');
  return (
    <Panel title="Validation" actions={<span className={`tag ${shown.some((i) => i.severity === 'error') ? 'bad' : 'good'}`}>{shown.length ? `${shown.length} issue(s)` : 'valid'}</span>}>
      {network.issues.length === 0 ? <div className="small muted">No issues: the destination is reachable in both directions.</div> : (
        <div className="issues">{network.issues.map((i, k) => <div key={k} className={`issue ${i.severity}`}>{i.message}</div>)}</div>
      )}
    </Panel>
  );
}
