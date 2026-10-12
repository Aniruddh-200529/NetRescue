#ifndef NETRESCUE_ARQ_H
#define NETRESCUE_ARQ_H

/*
 * Reliable-transfer (ARQ) engine.
 *
 * A discrete-event simulation of one sender -> receiver transfer across the NrNetwork graph:
 *   - every frame travels hop by hop along Dijkstra next hops;
 *   - every directed link endpoint ("port") has a bounded FIFO queue and serialises frames at the
 *     link bandwidth, then the frame propagates for delay_ms (+ optional jitter);
 *   - data and ACK/NAK frames carry an FNV-1a checksum over their header and payload bytes;
 *     corruption flips real bits and the receiver detects it by recomputing the checksum;
 *   - Stop-and-Wait, Go-Back-N and Selective Repeat sender/receiver state machines run on top.
 *
 * All random impairments are drawn from a counter-based hash of (seed, sequence, attempt, kind),
 * so the n-th transmission of packet k meets the same fate in every protocol run with that seed.
 */

#include "netrescue.h"
#include "congestion.h"

#define NR_ARQ_MAX_PACKETS 1024
#define NR_ARQ_HISTORY 256
#define NR_ARQ_LOG 128
#define NR_ARQ_HEADER_BYTES 20U       /* modelled data-frame header (sequence, ack, length, checksum, ...) */
#define NR_ARQ_ACK_BYTES 20U          /* modelled ACK/NAK frame size */
#define NR_ARQ_MAX_PAYLOAD 65000U
#define NR_ARQ_MAX_RULES 32
#define NR_ARQ_MAX_QUEUE 256U         /* hard bound on any port queue; also the endpoints' own NIC buffer */
#define NR_ARQ_DEFAULT_TIME_LIMIT 600.0
#define NR_ARQ_MONITOR_INTERVAL 0.01  /* congestion monitor / queue sampling period (s) */
#define NR_ARQ_TRACE_LIMIT 250000U
#define NR_ARQ_FAULT_LIMIT 100000U

typedef enum { NR_ARQ_STOP_WAIT, NR_ARQ_GO_BACK_N, NR_ARQ_SELECTIVE_REPEAT, NR_ARQ_PROTOCOL_COUNT } NrArqProtocol;
typedef enum { NR_ARQ_IDLE, NR_ARQ_RUNNING, NR_ARQ_PAUSED, NR_ARQ_COMPLETE, NR_ARQ_FAILED, NR_ARQ_STOPPED } NrArqState;
typedef enum { NR_ARQ_DATA_EVENT, NR_ARQ_ACK_EVENT, NR_ARQ_NAK_EVENT, NR_ARQ_CROSS_EVENT } NrArqEventType; /* frame kinds */
typedef enum { NR_CC_NONE, NR_CC_RATE_AIMD, NR_CC_WINDOW_AIMD, NR_CC_MODE_COUNT } NrCongestionMode;
typedef enum {
    NR_STOP_NONE, NR_STOP_COMPLETE, NR_STOP_RETRY_LIMIT, NR_STOP_TIME_LIMIT, NR_STOP_CAPACITY, NR_STOP_USER
} NrArqStopReason;

typedef struct {
    NrArqProtocol protocol;
    size_t packet_count;           /* original application packets to transfer */
    unsigned window_size;          /* forced to 1 for Stop-and-Wait */
    unsigned payload_bytes;
    double packets_per_second;     /* offered application rate; 0 sends nothing */
    double timeout_seconds;
    double loss_probability;       /* end-to-end random data-frame loss per transmission */
    double corruption_probability; /* end-to-end random data-frame corruption per transmission */
    double ack_loss_probability;
    double ack_corruption_probability;
    unsigned seed;
    bool congestion_control;       /* legacy switch, equivalent to congestion_mode = NR_CC_RATE_AIMD */
    NrCongestionMode congestion_mode;
    unsigned max_retries;          /* retransmissions allowed per packet before the transfer is aborted */
    double max_sim_time;           /* stopping criterion in simulated seconds; 0 selects the default */
    bool use_nak;                  /* receiver returns a NAK when it detects a corrupted data frame */
    unsigned corruption_bits;      /* bits flipped per corruption (0 is treated as 1) */
    double cross_traffic_pps;      /* constant-rate background frames at the first forwarding hop */
    unsigned cross_traffic_bytes;
} NrArqConfig;

/* ---- Deterministic fault scenarios ------------------------------------------------------- */
typedef enum {
    NR_RULE_DROP_DATA,     /* drop data seq on transmission `attempt` (0 = every attempt) */
    NR_RULE_CORRUPT_DATA,  /* corrupt data seq on transmission `attempt` */
    NR_RULE_DROP_ACK,      /* drop the `attempt`-th ACK/NAK the receiver sends in response to seq */
    NR_RULE_CORRUPT_ACK,   /* corrupt that ACK/NAK */
    NR_RULE_BURST_LOSS,    /* drop data seq .. seq+count-1 on transmission `attempt` */
    NR_RULE_LINK_OUTAGE,   /* link_id is unusable during [start, end) simulated seconds */
    NR_RULE_QUEUE_LIMIT,   /* node_id's forwarding queue is limited to `capacity` frames */
    NR_RULE_TYPE_COUNT
} NrFaultRuleType;

typedef struct {
    NrFaultRuleType type;
    unsigned sequence, attempt, count, capacity;
    int link_id, node_id;
    double start, end;
} NrFaultRule;

typedef struct { NrFaultRule rules[NR_ARQ_MAX_RULES]; size_t count; } NrFaultScenario;

/* ---- Fault ledger --------------------------------------------------------------------------- */
typedef enum {
    NR_FAULT_DATA_LOSS, NR_FAULT_DATA_CORRUPTION, NR_FAULT_ACK_LOSS, NR_FAULT_ACK_CORRUPTION,
    NR_FAULT_QUEUE_OVERFLOW, NR_FAULT_LINK_OUTAGE, NR_FAULT_NO_ROUTE, NR_FAULT_TYPE_COUNT
} NrFaultType;
typedef enum { NR_SRC_RANDOM, NR_SRC_LINK_PROBABILITY, NR_SRC_SCENARIO, NR_SRC_CONGESTION, NR_SRC_TOPOLOGY, NR_SRC_COUNT } NrFaultSource;
typedef enum {
    NR_RESP_PENDING, NR_RESP_NAK_SENT, NR_RESP_DISCARDED, NR_RESP_RETX_TIMEOUT, NR_RESP_RETX_NAK,
    NR_RESP_RETX_GOBACK, NR_RESP_COVERED_BY_ACK, NR_RESP_NO_IMPACT, NR_RESP_UNDETECTED, NR_RESP_COUNT
} NrFaultResponse;

typedef struct {
    double time, detected_at, recovered_at;
    NrFaultType type;
    NrFaultSource source;
    NrArqEventType frame_kind;
    uint32_t frame_id;
    unsigned sequence, attempt;
    int node_id, link_id;
    bool detected, recovered;
    NrFaultResponse response;
    int next_same_sequence; /* internal per-sequence chain */
} NrArqFault;

/* ---- Structured trace ----------------------------------------------------------------------- */
typedef enum {
    NR_TR_DATA_SEND, NR_TR_DATA_RETX, NR_TR_DATA_ACCEPTED, NR_TR_DATA_BUFFERED, NR_TR_DATA_DELIVERED,
    NR_TR_DATA_DUPLICATE, NR_TR_DATA_DISCARDED_OUT_OF_ORDER, NR_TR_DATA_OUT_OF_WINDOW, NR_TR_DATA_CORRUPT_DETECTED,
    NR_TR_DATA_UNDETECTED_CORRUPTION, NR_TR_DATA_LOST, NR_TR_ACK_SEND, NR_TR_NAK_SEND, NR_TR_ACK_RECEIVED,
    NR_TR_ACK_DUPLICATE, NR_TR_NAK_RECEIVED, NR_TR_ACK_LOST, NR_TR_ACK_CORRUPT_DETECTED, NR_TR_TIMEOUT,
    NR_TR_CORRUPTION_INJECTED, NR_TR_CONGESTION_DECREASE, NR_TR_CONGESTION_INCREASE, NR_TR_LINK_DOWN, NR_TR_LINK_UP,
    NR_TR_REROUTE, NR_TR_COMPLETE, NR_TR_FAILED, NR_TR_TIME_LIMIT, NR_TR_PAUSED, NR_TR_RESUMED, NR_TR_STOPPED,
    NR_TR_STARTED, NR_TR_TYPE_COUNT
} NrArqTraceType;
typedef enum { NR_CAUSE_NONE, NR_CAUSE_TIMEOUT, NR_CAUSE_NAK, NR_CAUSE_GOBACK } NrRetxCause;

typedef struct {
    double time;
    NrArqTraceType type;
    uint32_t frame_id;
    int sequence, ack, attempt;
    int node_id, link_id;
    int detail; /* retransmission cause, fault type, or congestion value depending on type */
    int source; /* fault source for loss records */
} NrArqTrace;

/* ---- Per-packet sender/receiver state ---------------------------------------------------- */
typedef struct {
    bool acknowledged, received, delivered, failed;
    unsigned attempts, timer_token, ack_occurrences, accepted_attempt; /* accepted_attempt: copy the receiver accepted */
    uint32_t checksum;
    double created_at, last_sent_at, received_at, delivered_at, acked_at;
    int fault_head;
} NrArqFrame;

typedef enum { NR_WIRE_FREE, NR_WIRE_QUEUED, NR_WIRE_TRANSMITTING, NR_WIRE_PROPAGATING } NrWireState;

/* One frame instance on the wire (a transmission of a data packet, an ACK/NAK, or cross traffic). */
typedef struct {
    uint32_t id;
    NrArqEventType kind;
    NrWireState state;
    unsigned sequence, ack, attempt, bytes;
    int origin, target, at_node, next_node, link_index;
    uint32_t checksum;
    bool corrupted;
    unsigned corrupt_key;
    int loss_hop, corrupt_hop;
    NrFaultSource loss_source, corrupt_source;
    unsigned hops_done;
    double sent_at, hop_start, hop_end;
} NrArqWire;

typedef struct {
    uint32_t *queue;
    unsigned head, count;
    int transmitting; /* wire pool index or -1 */
    double occupancy_area, last_change;
    unsigned max_occupancy;
    uint64_t drops, forwarded;
} NrArqPort;

typedef struct { double time; uint32_t order; uint32_t type, a, b; } NrArqScheduled;

typedef struct {
    double time_seconds, delivery_ratio, throughput_mbps, average_latency_ms, queue_utilization, packet_loss_rate;
    double window, rate;
    uint64_t retransmissions, delivered, in_flight;
} NrArqSample;

typedef struct {
    uint64_t generated;           /* first transmissions of original packets */
    uint64_t data_transmissions;  /* every data frame put on the wire by the sender */
    uint64_t delivered;           /* unique packets delivered in order to the receiving application */
    uint64_t received_unique;     /* unique packets accepted by the receiver (SR may buffer before delivery) */
    uint64_t packet_losses;       /* data frames that never reached the receiver (every cause) */
    uint64_t corruptions;         /* corrupted data frames detected by the receiver's checksum */
    uint64_t acknowledgements;    /* valid positive ACKs received by the sender */
    uint64_t negative_acks;       /* valid NAKs received by the sender */
    uint64_t ack_losses;          /* ACK/NAK frames that never reached the sender (every cause) */
    uint64_t ack_corruptions;     /* corrupted ACK/NAK frames detected and discarded by the sender */
    uint64_t retransmissions;     /* data retransmissions (attempt > 1) */
    uint64_t timeouts;            /* timer expirations that triggered retransmission */
    uint64_t out_of_order;        /* data frames arriving ahead of the receiver base */
    uint64_t duplicates;          /* data frames for packets already received */
    uint64_t queue_drops;         /* data + ACK frames dropped by full queues */
    double total_latency_ms, max_latency_ms, min_latency_ms, latency_sq_sum;
    uint64_t retx_timeout, retx_nak, retx_goback, retx_unnecessary;
    uint64_t acks_sent, naks_sent, dup_acks_sent, dup_acks_received;
    uint64_t data_bits_sent, control_bits_sent;
    uint64_t undetected_corruptions, out_of_window, data_queue_drops, ack_queue_drops;
    uint64_t rtt_samples; double rtt_sum_ms, rtt_min_ms, rtt_max_ms;
    uint64_t cross_traffic_frames, cross_traffic_drops;
    uint64_t congestion_decreases, congestion_increases;
    double min_rate, min_window;
} NrArqStatistics;

typedef struct {
    bool completed;
    double duration_seconds, efficiency;
    NrArqStatistics statistics;
} NrArqComparisonResult;

/* Fault summary per fault type, derived from the ledger. */
typedef struct { uint64_t injected, detected, recovered, unrecovered, no_impact; } NrFaultCounts;

/* Every metric is documented in docs/metrics.md and returned by nr_arq_metric_definitions(). */
typedef struct {
    bool completed;
    NrArqStopReason stop_reason;
    uint64_t packets_requested, packets_delivered, packets_undelivered;
    double elapsed_seconds;               /* completion time, or the time at which the run stopped */
    bool has_delivery_ratio, has_goodput, has_latency, has_retx_rate, has_efficiency, has_rtt;
    double delivery_ratio;                /* delivered / requested */
    double goodput_bps;                   /* delivered * payload * 8 / elapsed */
    double throughput_bps;                /* (data bits + control bits put on the wire by both endpoints) / elapsed */
    double data_throughput_bps;           /* data-frame bits incl. headers and retransmissions / elapsed */
    double latency_avg_ms, latency_min_ms, latency_max_ms, latency_stddev_ms;
    double retransmission_rate;           /* retransmissions / first transmissions */
    double efficiency;                    /* delivered payload bits / total bits put on the wire */
    double frame_efficiency;              /* delivered / data transmissions */
    double rtt_avg_ms;
    uint64_t retransmissions, retx_timeout, retx_nak, retx_goback, retx_unnecessary;
    uint64_t data_transmissions, first_transmissions, acks_sent, naks_sent, dup_acks_sent;
    uint64_t timeouts, duplicates_received, out_of_order_received;
    uint64_t data_bits, control_bits;
    NrFaultCounts faults[NR_FAULT_TYPE_COUNT];
    uint64_t faults_total, faults_recovered, faults_unrecovered;
    double queue_avg_occupancy, queue_max_avg_occupancy; /* time-averaged waiting frames: mean over used ports, worst port */
    unsigned queue_max_occupancy;
    uint64_t queue_drops, data_queue_drops, ack_queue_drops;
    uint64_t congestion_decreases, congestion_increases;
    double min_rate, min_window;
    uint64_t cross_traffic_frames, cross_traffic_drops;
} NrArqMetrics;

typedef struct {
    const NrNetwork *network;
    NrRoute forward_route, reverse_route;
    int source, destination;
    NrArqConfig config;
    NrFaultScenario scenario;
    NrArqFrame frames[NR_ARQ_MAX_PACKETS];
    bool receiver_buffer[NR_ARQ_MAX_PACKETS];
    size_t next_sequence, sender_base, receiver_base;
    size_t event_count;          /* pending scheduled events */
    double time_seconds, queue_utilization;
    NrCongestionController congestion;
    double cwnd, ssthresh, last_window_decrease;
    bool gbn_nak_sent;           /* Go-Back-N receiver NAKs the expected frame at most once */
    NrArqState state;
    NrArqStopReason stop_reason;
    NrArqStatistics stats;
    NrArqSample history[NR_ARQ_HISTORY];
    size_t history_count;
    unsigned history_version;    /* increments on every sample (lets clients poll cheaply) */
    double history_interval, next_sample_at;
    char log[NR_ARQ_LOG][NR_EVENT_LEN];
    size_t log_count, log_cursor;
    bool recording;              /* trace + text log; disabled by the experiment runner for speed */

    /* Discrete-event core (internal). */
    NrArqScheduled *heap; size_t heap_capacity; uint32_t event_order;
    NrArqWire *pool; uint32_t *pool_free; size_t pool_capacity, pool_free_count;
    NrArqPort *ports; size_t port_count;
    int next_hop_node[2][NR_MAX_NODES], next_hop_link[2][NR_MAX_NODES]; bool next_hop_valid[2][NR_MAX_NODES];
    bool link_blocked[NR_MAX_LINKS], link_was_active[NR_MAX_LINKS];
    double link_down_at[NR_MAX_LINKS];
    unsigned queue_override[NR_MAX_NODES];
    uint64_t topology_signature;
    uint32_t next_frame_id, gbn_timer_token;
    bool gbn_timer_running, wake_pending;
    double sender_next_allowed;
    unsigned char *scratch; size_t scratch_capacity;

    NrArqTrace *trace; size_t trace_count, trace_capacity, trace_dropped;
    NrArqFault *faults; size_t fault_count, fault_capacity; unsigned fault_version;
} NrArqSimulation;

bool nr_arq_start(NrArqSimulation *simulation, const NrNetwork *network,
                  const NrArqConfig *config, int source, int destination);
bool nr_arq_start_scenario(NrArqSimulation *simulation, const NrNetwork *network, const NrArqConfig *config,
                           const NrFaultScenario *scenario, int source, int destination);
void nr_arq_free(NrArqSimulation *simulation);
void nr_arq_step(NrArqSimulation *simulation, double delta_seconds);
bool nr_arq_step_event(NrArqSimulation *simulation); /* processes exactly one scheduled event (running or paused) */
void nr_arq_run(NrArqSimulation *simulation);        /* runs until complete, failed or the time limit */
void nr_arq_pause(NrArqSimulation *simulation);
void nr_arq_resume(NrArqSimulation *simulation);
void nr_arq_stop(NrArqSimulation *simulation);
void nr_arq_topology_changed(NrArqSimulation *simulation);
void nr_arq_log(NrArqSimulation *simulation, const char *message);
void nr_arq_default_config(NrArqConfig *config);
const char *nr_arq_protocol_name(NrArqProtocol protocol);
const char *nr_arq_state_name(NrArqState state);
const char *nr_arq_stop_reason_name(NrArqStopReason reason);
const char *nr_arq_trace_name(NrArqTraceType type);
const char *nr_fault_type_name(NrFaultType type);
const char *nr_fault_source_name(NrFaultSource source);
const char *nr_fault_response_name(NrFaultResponse response);
const char *nr_fault_rule_name(NrFaultRuleType type);
const char *nr_congestion_mode_name(NrCongestionMode mode);
const char *nr_retx_cause_name(NrRetxCause cause);
void nr_arq_trace_describe(const NrArqSimulation *simulation, const NrArqTrace *record, char *buffer, size_t size);
double nr_arq_delivery_ratio(const NrArqSimulation *simulation);
double nr_arq_throughput_mbps(const NrArqSimulation *simulation);
double nr_arq_efficiency(const NrArqSimulation *simulation);
double nr_arq_packet_progress(const NrArqSimulation *simulation, unsigned sequence, bool *corrupted);
unsigned nr_arq_port_capacity(const NrArqSimulation *simulation, size_t port_index);
unsigned nr_arq_effective_window(const NrArqSimulation *simulation);
void nr_arq_compute_metrics(const NrArqSimulation *simulation, NrArqMetrics *metrics);
bool nr_arq_compare(const NrNetwork *network, const NrArqConfig *config, int source, int destination,
                    NrArqComparisonResult *go_back_n, NrArqComparisonResult *selective_repeat);
bool nr_scenario_add(NrFaultScenario *scenario, const NrFaultRule *rule);

#endif
