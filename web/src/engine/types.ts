// TypeScript mirror of the JSON produced by the C engine (src/api.c, documented in docs/api.md).
// These are types only: every value shown in the UI comes from the engine.

export type ProtocolId = 'stop_and_wait' | 'go_back_n' | 'selective_repeat';
export type NodeType = 'host' | 'router' | 'switch' | 'server' | 'access_point';
export type TopologyType = 'line' | 'bus' | 'star' | 'ring' | 'tree' | 'mesh' | 'hybrid' | 'random';
export type CongestionMode = 'none' | 'rate_aimd' | 'window_aimd';
export type RuleType = 'drop_data' | 'corrupt_data' | 'drop_ack' | 'corrupt_ack' | 'burst_loss' | 'link_outage' | 'queue_limit';

export interface NetNode { id: number; name: string; type: NodeType; x: number; y: number; active: boolean; ip: string; queue_capacity: number }
export interface NetLink {
  id: number; from: number; to: number; bidirectional: boolean; active: boolean;
  cost: number; delay_ms: number; bandwidth_mbps: number; loss_probability: number; jitter_ms: number;
}
export interface Issue { severity: 'info' | 'warning' | 'error'; message: string; node_id: number; link_id: number }
export interface NetworkState {
  nodes: NetNode[]; links: NetLink[]; source: number; destination: number;
  route: number[]; reverse_route: number[]; route_cost: number | null; cost_mode: 'link' | 'delay' | 'composite';
  locked: boolean; base_rtt_ms: number | null; issues: Issue[];
}

export interface ArqConfig {
  protocol: ProtocolId; packet_count: number; window_size: number; payload_bytes: number; packets_per_second: number;
  timeout_s: number; loss_probability: number; corruption_probability: number; ack_loss_probability: number;
  ack_corruption_probability: number; seed: number; max_retries: number; max_sim_time: number;
  congestion_mode: CongestionMode; use_nak: boolean; corruption_bits: number; cross_traffic_pps: number; cross_traffic_bytes: number;
}

export interface FaultRule {
  type: RuleType; sequence?: number; attempt?: number; count?: number; link_id?: number; node_id?: number;
  start?: number; end?: number; capacity?: number;
}

export interface FaultCounts { injected: number; detected: number; recovered: number; unrecovered: number; no_impact: number }
export type FaultTypeId = 'data_loss' | 'data_corruption' | 'ack_loss' | 'ack_corruption' | 'queue_overflow' | 'link_outage' | 'no_route';

export interface Metrics {
  completed: boolean; stop_reason: string; packets_requested: number; packets_delivered: number; packets_undelivered: number;
  elapsed_s: number; delivery_ratio: number | null; goodput_bps: number | null; throughput_bps: number | null;
  data_throughput_bps: number | null; latency_avg_ms: number | null; latency_min_ms: number | null; latency_max_ms: number | null;
  latency_stddev_ms: number | null; retransmission_rate: number | null; efficiency: number | null; frame_efficiency: number | null;
  rtt_avg_ms: number | null; retransmissions: number; retx_timeout: number; retx_nak: number; retx_goback: number;
  retx_unnecessary: number; data_transmissions: number; first_transmissions: number; acks_sent: number; naks_sent: number;
  dup_acks_sent: number; timeouts: number; duplicates_received: number; out_of_order_received: number;
  data_bits: number; control_bits: number; faults: Record<FaultTypeId, FaultCounts>; faults_total: number;
  faults_recovered: number; faults_unrecovered: number; queue_avg_occupancy: number; queue_max_avg_occupancy: number;
  queue_max_occupancy: number; queue_drops: number; data_queue_drops: number; ack_queue_drops: number;
  congestion_decreases: number; congestion_increases: number; min_rate: number; min_window: number;
  cross_traffic_frames: number; cross_traffic_drops: number;
}

export interface TraceRecord {
  i: number; t: number; type: string; seq: number; ack: number; attempt: number; frame: number; node: number; link: number;
  detail: number; text: string; cause?: string; fault_source?: string; fault_type?: string;
}

export interface FaultRecord {
  i: number; t: number; type: FaultTypeId; source: string; frame_kind: 'data' | 'ack' | 'nak'; frame: number; seq: number;
  attempt: number; node: number; link: number; detected: boolean; detected_at: number | null; response: string;
  recovered: boolean; recovered_at: number | null;
}

export interface HistorySample {
  t: number; delivery_ratio: number; goodput_mbps: number; latency_ms: number; queue_utilization: number; loss_rate: number;
  window: number; rate: number; retransmissions: number; delivered: number; outstanding: number;
}

export interface WireFrame {
  id: number; kind: 'data' | 'ack' | 'nak' | 'cross'; seq: number; ack: number; attempt: number;
  phase: 'queued' | 'transmitting' | 'propagating'; from: number; to: number; link: number; progress: number; corrupted: boolean;
}

export interface PortState { link: number; node: number; toward: number; queued: number; capacity: number; transmitting: boolean; max: number; drops: number; forwarded: number }

export interface SimState {
  state: 'Idle' | 'Running' | 'Paused' | 'Complete' | 'Failed' | 'Stopped'; active: boolean;
  stop_reason?: string; time?: number; protocol?: ProtocolId; pending_events?: number; config?: ArqConfig;
  sender?: { base: number; next: number; window: number; effective_window: number; cwnd: number; rate: number; gbn_timer_running: boolean };
  receiver?: { base: number }; sender_map?: string; receiver_map?: string; attempts_map?: string;
  route?: number[]; reverse_route?: number[]; blocked_links?: number[]; in_flight?: WireFrame[]; ports?: PortState[];
  metrics?: Metrics; trace_total?: number; trace_from?: number; trace_dropped?: number; trace?: TraceRecord[];
  fault_version?: number; faults?: FaultRecord[]; history_version?: number; history?: HistorySample[];
}

export interface Stat { mean: number | null; stddev: number | null; ci95: number | null; min: number | null; max: number | null; n: number }

export interface ExperimentSpec {
  config: ArqConfig; scenario: FaultRule[]; protocols: ProtocolId[]; trials: number; seed_base: number; seed_stride: number;
  seed_schedule?: string; sweep: { parameter: string; values: number[] };
  overrides: { link_delay_ms: number | null; link_bandwidth_mbps: number | null; link_jitter_ms: number | null; queue_capacity: number | null };
  capture_trace: boolean; source: number; destination: number;
}

export interface ExperimentRun {
  index: number; sweep_index: number; sweep_value: number | null; protocol: ProtocolId; trial: number; seed: number;
  metrics: Metrics; retransmitted: { seq: number; count: number }[]; retransmitted_sequences: number; captured: boolean;
}

export interface ExperimentAggregate {
  sweep_index: number; sweep_value: number | null; protocol: ProtocolId; runs: number; completed: number; stats: Record<string, Stat>;
}

export interface ExperimentCapture { run_index: number; protocol: ProtocolId; sweep_index: number; trace_total: number; trace: TraceRecord[]; faults: FaultRecord[] }

export interface ExperimentResult {
  api_version: number; spec: ExperimentSpec;
  topology: Omit<NetworkState, 'cost_mode' | 'locked' | 'issues' | 'base_rtt_ms'> & { description: string; base_rtt_ms: number; route_names: string[] };
  metric_keys: string[]; runs: ExperimentRun[]; aggregates: ExperimentAggregate[]; findings: string[]; captures: ExperimentCapture[];
}

export interface PresetInfo { id: string; title: string; purpose: string; settings: string; what_to_look_for: string }
export interface MetricDefinition { key: string; name: string; unit: string; formula: string; description: string }
export interface EngineVersion { engine: string; api_version: number; target: string; built: string; limits: Record<string, number> }

export interface ApiResponse<T> { ok: boolean; api_version: number; result?: T; error?: string }
