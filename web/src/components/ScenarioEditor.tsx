import { useState } from 'react';
import type { FaultRule, NetLink, NetNode, RuleType } from '../engine/types';

const RULES: { type: RuleType; label: string; help: string }[] = [
  { type: 'drop_data', label: 'Drop DATA packet', help: 'Drops data sequence N on transmission attempt A (1 = first send, 0 = every send), on the middle hop of the route.' },
  { type: 'corrupt_data', label: 'Corrupt DATA packet', help: 'Flips payload bits of sequence N on attempt A; the receiver checksum must catch it.' },
  { type: 'drop_ack', label: 'Drop ACK', help: 'Drops the A-th ACK/NAK the receiver sends in response to sequence N (the data itself arrived).' },
  { type: 'corrupt_ack', label: 'Corrupt ACK', help: 'Corrupts that ACK/NAK; the sender checksum discards it.' },
  { type: 'burst_loss', label: 'Burst loss', help: 'Drops COUNT consecutive data packets starting at sequence N on attempt A.' },
  { type: 'link_outage', label: 'Temporary link outage', help: 'The link is down between START and END (simulated seconds). Frames on it are lost and routing recalculates.' },
  { type: 'queue_limit', label: 'Force queue overflow', help: "Limits a router's forwarding queue to CAPACITY frames for this run." },
];

export function describeRule(rule: FaultRule, links: NetLink[] = [], nodes: NetNode[] = []): string {
  const attempt = rule.attempt === 0 ? 'every attempt' : `attempt ${rule.attempt ?? 1}`;
  switch (rule.type) {
    case 'drop_data': return `Drop DATA seq ${rule.sequence} (${attempt})`;
    case 'corrupt_data': return `Corrupt DATA seq ${rule.sequence} (${attempt})`;
    case 'drop_ack': return `Drop ACK for seq ${rule.sequence} (${rule.attempt === 0 ? 'every ACK' : `ACK #${rule.attempt ?? 1}`})`;
    case 'corrupt_ack': return `Corrupt ACK for seq ${rule.sequence} (${rule.attempt === 0 ? 'every ACK' : `ACK #${rule.attempt ?? 1}`})`;
    case 'burst_loss': return `Drop DATA seq ${rule.sequence}..${(rule.sequence ?? 0) + (rule.count ?? 1) - 1} (${attempt})`;
    case 'link_outage': {
      const link = links.find((l) => l.id === rule.link_id);
      const name = (id: number) => nodes.find((n) => n.id === id)?.name ?? id;
      return `Link ${rule.link_id}${link ? ` (${name(link.from)}–${name(link.to)})` : ''} down ${rule.start}s → ${rule.end}s`;
    }
    case 'queue_limit': return `Queue at ${nodes.find((n) => n.id === rule.node_id)?.name ?? `node ${rule.node_id}`} limited to ${rule.capacity}`;
  }
}

export function ScenarioEditor({ rules, onChange, links, nodes, disabled }: {
  rules: FaultRule[]; onChange: (rules: FaultRule[]) => void; links: NetLink[]; nodes: NetNode[]; disabled?: boolean;
}) {
  const [type, setType] = useState<RuleType>('drop_data');
  const [sequence, setSequence] = useState(3);
  const [attempt, setAttempt] = useState(1);
  const [count, setCount] = useState(3);
  const [linkId, setLinkId] = useState<number>(links[0]?.id ?? 1);
  const [nodeId, setNodeId] = useState<number>(nodes.find((n) => n.type === 'router' || n.type === 'switch')?.id ?? nodes[0]?.id ?? 1);
  const [start, setStart] = useState(0.1);
  const [end, setEnd] = useState(0.5);
  const [capacity, setCapacity] = useState(1);
  const meta = RULES.find((r) => r.type === type)!;

  const add = () => {
    let rule: FaultRule;
    if (type === 'link_outage') rule = { type, link_id: linkId, start, end };
    else if (type === 'queue_limit') rule = { type, node_id: nodeId, capacity };
    else if (type === 'burst_loss') rule = { type, sequence, count, attempt };
    else rule = { type, sequence, attempt };
    onChange([...rules, rule]);
  };

  return (
    <div>
      {rules.length === 0 && <p className="muted small" style={{ marginTop: 0 }}>No deterministic faults. Random impairments still apply.</p>}
      {rules.map((rule, i) => (
        <div className="rule-row" key={i}>
          <span>{describeRule(rule, links, nodes)}</span>
          <button className="btn ghost sm" disabled={disabled} aria-label="Remove rule" onClick={() => onChange(rules.filter((_, k) => k !== i))}>✕</button>
        </div>
      ))}
      <div className="field">
        <label>Add deterministic fault</label>
        <select value={type} disabled={disabled} onChange={(e) => setType(e.target.value as RuleType)} aria-label="Fault rule type">
          {RULES.map((r) => <option key={r.type} value={r.type}>{r.label}</option>)}
        </select>
        <span className="hint">{meta.help}</span>
      </div>
      {type === 'link_outage' ? (
        <div className="grid3">
          <div className="field"><label>Link</label>
            <select value={linkId} disabled={disabled} onChange={(e) => setLinkId(Number(e.target.value))}>
              {links.map((l) => <option key={l.id} value={l.id}>{l.id}: {nodes.find((n) => n.id === l.from)?.name}–{nodes.find((n) => n.id === l.to)?.name}</option>)}
            </select></div>
          <div className="field"><label>Start (s)</label><input type="number" step={0.05} min={0} value={start} disabled={disabled} onChange={(e) => setStart(e.target.valueAsNumber || 0)} /></div>
          <div className="field"><label>End (s)</label><input type="number" step={0.05} min={0} value={end} disabled={disabled} onChange={(e) => setEnd(e.target.valueAsNumber || 0)} /></div>
        </div>
      ) : type === 'queue_limit' ? (
        <div className="grid2">
          <div className="field"><label>Node</label>
            <select value={nodeId} disabled={disabled} onChange={(e) => setNodeId(Number(e.target.value))}>
              {nodes.map((n) => <option key={n.id} value={n.id}>{n.name}</option>)}
            </select></div>
          <div className="field"><label>Capacity</label><input type="number" min={1} max={256} value={capacity} disabled={disabled} onChange={(e) => setCapacity(Math.max(1, e.target.valueAsNumber || 1))} /></div>
        </div>
      ) : (
        <div className={type === 'burst_loss' ? 'grid3' : 'grid2'}>
          <div className="field"><label>Sequence</label><input type="number" min={0} value={sequence} disabled={disabled} onChange={(e) => setSequence(Math.max(0, e.target.valueAsNumber || 0))} /></div>
          {type === 'burst_loss' && <div className="field"><label>Count</label><input type="number" min={1} value={count} disabled={disabled} onChange={(e) => setCount(Math.max(1, e.target.valueAsNumber || 1))} /></div>}
          <div className="field"><label>{type.endsWith('ack') ? 'ACK #' : 'Attempt'}</label><input type="number" min={0} value={attempt} disabled={disabled} onChange={(e) => setAttempt(Math.max(0, e.target.valueAsNumber || 0))} /></div>
        </div>
      )}
      <button className="btn sm" disabled={disabled} onClick={add}>+ Add fault rule</button>
    </div>
  );
}
