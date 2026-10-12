// Interactive topology view. Node/link positions and every packet drawn here come from the C engine's
// state (get_network / sim_state in_flight); the canvas only maps them to screen coordinates.
import { memo, useCallback, useEffect, useMemo } from 'react';
import {
  BaseEdge, Controls, Handle, Position, ReactFlow, ReactFlowProvider, useNodesState, useReactFlow,
  type Edge, type EdgeProps, type Node, type NodeProps,
} from '@xyflow/react';
import type { NetLink, NetNode, PortState, WireFrame } from '../../engine/types';
import { useLab, type VisualEffect } from '../../state/labStore';

const W = 1000, H = 640;
const NO_PORTS: PortState[] = [];
const TYPE_STYLE: Record<NetNode['type'], { color: string; glyph: string }> = {
  host: { color: '#3fa7ff', glyph: 'PC' }, router: { color: '#ef7d32', glyph: 'R' }, switch: { color: '#2fbf71', glyph: 'SW' },
  server: { color: '#b18cff', glyph: 'SV' }, access_point: { color: '#4dd0e1', glyph: 'AP' },
};

type NetNodeData = { node: NetNode; role: 'source' | 'destination' | null; selected: boolean; queued: number; capacity: number; drops: number; onRoute: boolean };
type NetEdgeData = { link: NetLink; onRoute: boolean; blocked: boolean; selected: boolean; frames: WireFrame[]; effects: VisualEffect[]; showLabels: boolean };

const NetNodeView = memo(({ data }: NodeProps<Node<NetNodeData>>) => {
  const { node, role, selected, queued, capacity, drops } = data;
  const style = TYPE_STYLE[node.type];
  return (
    <div className={`netnode ${selected ? 'selected' : ''} ${node.active ? '' : 'failed'}`} title={`${node.name} (${node.type}) ${node.ip}\nqueue capacity ${node.queue_capacity}`}>
      <div className="disc" style={{ borderColor: node.active ? style.color : 'var(--critical)', color: style.color }}>
        <b style={{ fontSize: 12 }}>{style.glyph}</b>
        {role && <span className="role" style={{ background: role === 'source' ? '#1f6fd1' : '#1d7a4c', color: '#fff' }}>{role === 'source' ? 'SRC' : 'DST'}</span>}
        <Handle type="source" position={Position.Top} isConnectable={false} style={{ top: '50%', left: '50%' }} />
        <Handle type="target" position={Position.Top} isConnectable={false} style={{ top: '50%', left: '50%' }} />
      </div>
      <span className="name">{node.name}</span>
      {(queued > 0 || drops > 0) && (
        <span className="mono small" style={{ color: queued >= capacity && capacity > 0 ? 'var(--critical)' : 'var(--warning)' }}>
          q {queued}/{capacity}{drops > 0 ? ` · ${drops} drop` : ''}
        </span>
      )}
    </div>
  );
});

function framePosition(sx: number, sy: number, tx: number, ty: number, f: WireFrame, linkFrom: number) {
  const p = f.phase === 'propagating' ? f.progress : f.phase === 'transmitting' ? 0.04 + 0.04 * f.progress : 0.02;
  const forward = f.from === linkFrom;
  const t = forward ? p : 1 - p;
  const x = sx + (tx - sx) * t, y = sy + (ty - sy) * t;
  // Offset the two directions so DATA and ACK traffic do not overlap.
  const len = Math.hypot(tx - sx, ty - sy) || 1;
  const nx = -(ty - sy) / len, ny = (tx - sx) / len;
  const off = forward ? -9 : 9;
  return { x: x + nx * off, y: y + ny * off };
}

const PacketEdge = memo(({ id, sourceX, sourceY, targetX, targetY, data }: EdgeProps<Edge<NetEdgeData>>) => {
  if (!data) return null;
  const { link, onRoute, blocked, selected, frames, effects, showLabels } = data;
  const path = `M ${sourceX} ${sourceY} L ${targetX} ${targetY}`;
  const color = !link.active ? 'var(--critical)' : blocked ? 'var(--warning)' : onRoute ? 'var(--accent)' : '#3b5272';
  const mx = (sourceX + targetX) / 2, my = (sourceY + targetY) / 2;
  return (
    <>
      {onRoute && link.active && !blocked && <path d={path} stroke="rgba(63,167,255,0.18)" strokeWidth={10} fill="none" />}
      <BaseEdge id={id} path={path} style={{ stroke: color, strokeWidth: selected ? 4 : onRoute ? 2.5 : 1.6, strokeDasharray: !link.active || blocked ? '6 5' : undefined }} interactionWidth={18} />
      {!link.active && <text x={mx} y={my + 6} textAnchor="middle" fontSize={20} fill="var(--critical)" fontWeight={700}>✕</text>}
      {showLabels && link.active && (
        <text x={mx} y={my - 10} textAnchor="middle" fontSize={10} fill="var(--text-muted)" style={{ paintOrder: 'stroke', stroke: '#0b1320', strokeWidth: 3 }}>
          {link.delay_ms}ms · {link.bandwidth_mbps}Mb{link.loss_probability > 0 ? ` · ${(link.loss_probability * 100).toFixed(0)}%loss` : ''}
        </text>
      )}
      {frames.map((f) => {
        const { x, y } = framePosition(sourceX, sourceY, targetX, targetY, f, link.from);
        if (f.kind === 'cross') return <circle key={f.id} cx={x} cy={y} r={3} fill="#5b6b82" />;
        if (f.kind === 'data') {
          const fill = f.corrupted ? 'var(--corrupt)' : f.attempt > 1 ? 'var(--retx)' : 'var(--data)';
          return (
            <g key={f.id}>
              <circle cx={x} cy={y} r={10} fill={fill} stroke="#0b1320" strokeWidth={2} />
              <text x={x} y={y + 3.5} textAnchor="middle" fontSize={9.5} fontWeight={700} fill="#06101c">{f.seq}</text>
            </g>
          );
        }
        const fill = f.corrupted ? 'var(--corrupt)' : f.kind === 'nak' ? 'var(--nak)' : 'var(--ack)';
        return (
          <g key={f.id}>
            <rect x={x - 12} y={y - 7} width={24} height={14} rx={4} fill={fill} stroke="#0b1320" strokeWidth={1.5} />
            <text x={x} y={y + 3.5} textAnchor="middle" fontSize={8.5} fontWeight={700} fill="#06101c">{f.kind === 'nak' ? `N${f.seq}` : `A${f.ack}`}</text>
          </g>
        );
      })}
      {effects.map((e) => {
        const towardTarget = e.node === link.to;
        const t = towardTarget ? 0.8 : 0.2;
        const ex = sourceX + (targetX - sourceX) * t, ey = sourceY + (targetY - sourceY) * t;
        const c = e.kind === 'corrupt' ? 'var(--corrupt)' : e.kind === 'queue' ? 'var(--warning)' : 'var(--critical)';
        return (
          <g key={e.key}>
            <text x={ex} y={ey + 7} textAnchor="middle" fontSize={22} fontWeight={800} fill={c}>{e.kind === 'corrupt' ? '⚡' : '✕'}</text>
            <text x={ex} y={ey - 14} textAnchor="middle" fontSize={10} fill={c} style={{ paintOrder: 'stroke', stroke: '#0b1320', strokeWidth: 3 }}>{e.label}</text>
          </g>
        );
      })}
    </>
  );
});

const nodeTypes = { net: NetNodeView };
const edgeTypes = { packet: PacketEdge };

function onRouteEdge(route: number[] | undefined, a: number, b: number) {
  if (!route) return false;
  for (let i = 0; i + 1 < route.length; i++) if ((route[i] === a && route[i + 1] === b) || (route[i] === b && route[i + 1] === a)) return true;
  return false;
}

function CanvasInner({ showLabels }: { showLabels: boolean }) {
  const { network, sim, selection, effects, linkMode, select, networkCommand, setLinkMode, notify } = useLab();
  const { fitView } = useReactFlow();
  const route = sim?.active || sim?.route ? sim?.route ?? network?.route : network?.route;
  const ports: PortState[] = sim?.ports ?? NO_PORTS;

  const baseNodes = useMemo<Node<NetNodeData>[]>(() => (network?.nodes ?? []).map((n) => {
    const nodePorts = ports.filter((p) => p.node === n.id && p.node !== network?.source && p.node !== network?.destination);
    return {
      id: String(n.id), type: 'net', position: { x: n.x * W, y: n.y * H },
      data: {
        node: n, role: n.id === network?.source ? 'source' : n.id === network?.destination ? 'destination' : null,
        selected: selection?.kind === 'node' && selection.id === n.id,
        queued: nodePorts.reduce((s, p) => s + p.queued, 0), capacity: nodePorts.length ? Math.max(...nodePorts.map((p) => p.capacity)) : n.queue_capacity,
        drops: nodePorts.reduce((s, p) => s + p.drops, 0), onRoute: !!route?.includes(n.id),
      },
    };
  }), [network, ports, selection, route]);

  const [nodes, setNodes, onNodesChange] = useNodesState<Node<NetNodeData>>(baseNodes);
  // Merge engine data into React Flow's node objects so measurements (and an in-progress drag) survive updates.
  useEffect(() => {
    setNodes((previous) => baseNodes.map((node) => {
      const old = previous.find((p) => p.id === node.id);
      if (!old) return node;
      return { ...old, data: node.data, position: old.dragging ? old.position : node.position };
    }));
  }, [baseNodes, setNodes]);
  const nodeCount = network?.nodes.length ?? 0;
  const topologyKey = network?.nodes.map((n) => n.id).join(',');
  useEffect(() => { const t = setTimeout(() => fitView({ padding: 0.15, duration: 250 }), 50); return () => clearTimeout(t); }, [topologyKey, nodeCount, fitView]);

  const edges = useMemo<Edge<NetEdgeData>[]>(() => (network?.links ?? []).map((l) => ({
    id: String(l.id), source: String(l.from), target: String(l.to), type: 'packet',
    data: {
      link: l, onRoute: onRouteEdge(route, l.from, l.to) || onRouteEdge(sim?.reverse_route ?? network?.reverse_route, l.from, l.to),
      blocked: !!sim?.blocked_links?.includes(l.id), selected: selection?.kind === 'link' && selection.id === l.id,
      frames: (sim?.in_flight ?? []).filter((f) => f.link === l.id && f.phase !== 'queued'),
      effects: effects.filter((e) => e.link === l.id), showLabels,
    },
  })), [network, sim, route, selection, effects, showLabels]);

  const onNodeClick = useCallback((_: unknown, node: Node) => {
    const id = Number(node.id);
    if (linkMode !== null) {
      if (linkMode !== id) void networkCommand('add_link', { from: linkMode, to: id }).then((ok) => ok && notify('info', 'Link added'));
      setLinkMode(null);
      return;
    }
    select({ kind: 'node', id });
  }, [linkMode, networkCommand, notify, select, setLinkMode]);

  return (
    <ReactFlow
      nodes={nodes} edges={edges} nodeTypes={nodeTypes} edgeTypes={edgeTypes}
      onNodesChange={onNodesChange}
      onNodeClick={onNodeClick}
      onEdgeClick={(_, edge) => select({ kind: 'link', id: Number(edge.id) })}
      onPaneClick={() => { select(null); setLinkMode(null); }}
      onNodeDragStop={(_, node) => {
        void networkCommand('update_node', { id: Number(node.id), x: Math.min(1, Math.max(0, node.position.x / W)), y: Math.min(1, Math.max(0, node.position.y / H)) });
      }}
      nodesConnectable={false} minZoom={0.3} maxZoom={2.5} proOptions={{ hideAttribution: true }}
      className={linkMode !== null ? 'link-mode' : ''}
    >
      <Controls showInteractive={false} position="bottom-right" />
    </ReactFlow>
  );
}

export function TopologyCanvas({ showLabels }: { showLabels: boolean }) {
  return <ReactFlowProvider><CanvasInner showLabels={showLabels} /></ReactFlowProvider>;
}
