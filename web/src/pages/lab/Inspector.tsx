import { useEffect, useState } from 'react';
import type { NetLink, NetNode, NodeType } from '../../engine/types';
import { Panel } from '../../components/Fields';
import { useLab } from '../../state/labStore';

/** Edits are committed to the engine on blur/enter; the engine validates and returns the updated network. */
function CommitNumber({ label, value, min, max, step, onCommit, disabled, suffix }: {
  label: string; value: number; min: number; max: number; step: number; onCommit: (v: number) => void; disabled?: boolean; suffix?: string;
}) {
  const [text, setText] = useState(String(value));
  useEffect(() => setText(String(value)), [value]);
  const commit = () => {
    const v = Number(text);
    if (Number.isFinite(v) && v >= min && v <= max && v !== value) onCommit(v); else setText(String(value));
  };
  return (
    <div className="field">
      <label>{label}{suffix && <span className="muted">{suffix}</span>}</label>
      <input type="number" value={text} min={min} max={max} step={step} disabled={disabled} aria-label={label}
        onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => e.key === 'Enter' && commit()} />
    </div>
  );
}

function NodeInspector({ node }: { node: NetNode }) {
  const { networkCommand, sim, network } = useLab();
  const locked = !!sim?.active;
  const [name, setName] = useState(node.name);
  useEffect(() => setName(node.name), [node.name]);
  const port = sim?.ports?.filter((p) => p.node === node.id) ?? [];
  return (
    <>
      <div className="field"><label>Name</label>
        <input type="text" value={name} disabled={locked} maxLength={31} onChange={(e) => setName(e.target.value)}
          onBlur={() => name.trim() && name !== node.name && void networkCommand('update_node', { id: node.id, name: name.trim() })} /></div>
      <div className="grid2">
        <div className="field"><label>Type</label>
          <select value={node.type} disabled={locked} onChange={(e) => void networkCommand('update_node', { id: node.id, type: e.target.value as NodeType })}>
            <option value="host">Host</option><option value="router">Router</option><option value="switch">Switch</option>
            <option value="server">Server</option><option value="access_point">Access point</option>
          </select></div>
        <CommitNumber label="Queue capacity" value={node.queue_capacity} min={0} max={256} step={1} onCommit={(v) => void networkCommand('update_node', { id: node.id, queue_capacity: v })} />
      </div>
      <div className="small secondary" style={{ marginBottom: 8 }}>IP {node.ip} · id {node.id}{node.id === network?.source ? ' · source' : node.id === network?.destination ? ' · destination' : ''}</div>
      {port.length > 0 && (
        <div className="small" style={{ marginBottom: 8 }}>
          {port.map((p) => <div key={`${p.link}-${p.toward}`} className="mono">→ {network?.nodes.find((n) => n.id === p.toward)?.name}: queued {p.queued}/{p.capacity}, peak {p.max}, drops {p.drops}, sent {p.forwarded}</div>)}
        </div>
      )}
      <div className="btn-row">
        <button className={`btn sm ${node.active ? 'danger' : ''}`} onClick={() => void networkCommand('update_node', { id: node.id, active: !node.active })}>{node.active ? 'Fail node' : 'Recover node'}</button>
        <button className="btn sm" disabled={locked} onClick={() => void networkCommand('remove_node', { id: node.id })}>Delete</button>
      </div>
    </>
  );
}

function LinkInspector({ link }: { link: NetLink }) {
  const { networkCommand, network, sim } = useLab();
  const locked = !!sim?.active;
  const name = (id: number) => network?.nodes.find((n) => n.id === id)?.name ?? id;
  const update = (patch: Record<string, unknown>) => void networkCommand('update_link', { id: link.id, ...patch });
  return (
    <>
      <div className="small secondary" style={{ marginBottom: 8 }}>Link {link.id}: {name(link.from)} {link.bidirectional ? '⟷' : '→'} {name(link.to)}</div>
      <div className="grid2">
        <CommitNumber label="Bandwidth" suffix="Mbit/s" value={link.bandwidth_mbps} min={0} max={100000} step={0.1} onCommit={(v) => update({ bandwidth_mbps: v })} />
        <CommitNumber label="Delay" suffix="ms" value={link.delay_ms} min={0} max={60000} step={1} onCommit={(v) => update({ delay_ms: v })} />
        <CommitNumber label="Jitter (max)" suffix="ms" value={link.jitter_ms} min={0} max={60000} step={0.5} onCommit={(v) => update({ jitter_ms: v })} />
        <CommitNumber label="Link loss" suffix="0–1" value={link.loss_probability} min={0} max={1} step={0.01} onCommit={(v) => update({ loss_probability: v })} />
        <CommitNumber label="Routing cost" value={link.cost} min={0} max={1e6} step={1} onCommit={(v) => update({ cost: v })} />
        <div className="field"><label>Direction</label>
          <select value={link.bidirectional ? 'bi' : 'uni'} disabled={locked} onChange={(e) => update({ bidirectional: e.target.value === 'bi' })}>
            <option value="bi">Bidirectional</option><option value="uni">One-way</option>
          </select></div>
      </div>
      <div className="btn-row">
        <button className={`btn sm ${link.active ? 'danger' : ''}`} onClick={() => update({ active: !link.active })}>{link.active ? 'Fail link' : 'Recover link'}</button>
        <button className="btn sm" disabled={locked} onClick={() => void networkCommand('remove_link', { id: link.id })}>Delete</button>
      </div>
      <p className="hint muted small">Failing a link during a transfer drops frames on it and makes the engine reroute.</p>
    </>
  );
}

export function Inspector() {
  const { selection, network } = useLab();
  const node = selection?.kind === 'node' ? network?.nodes.find((n) => n.id === selection.id) : undefined;
  const link = selection?.kind === 'link' ? network?.links.find((l) => l.id === selection.id) : undefined;
  return (
    <Panel title={node ? `Node ${node.name}` : link ? `Link ${link.id}` : 'Inspector'}>
      {node ? <NodeInspector node={node} /> : link ? <LinkInspector link={link} /> : <div className="small muted">Click a node or link on the canvas to inspect and edit it. Drag nodes to move them.</div>}
    </Panel>
  );
}
