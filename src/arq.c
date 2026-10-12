#include "arq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scheduled event kinds of the discrete-event core. */
enum { EV_SENDER_WAKE, EV_TX_DONE, EV_ARRIVE, EV_TIMEOUT, EV_GBN_TIMEOUT, EV_MONITOR, EV_OUTAGE_START, EV_OUTAGE_END, EV_CROSS };

/* Purposes of the counter-based random draws. 0-3 keep the numbering of the original engine. */
enum { K_DATA_LOSS = 0, K_DATA_CORRUPT = 1, K_ACK_LOSS = 2, K_ACK_CORRUPT = 3, K_LOSS_HOP = 4, K_CORRUPT_HOP = 5,
       K_JITTER = 6, K_LINK_LOSS = 7, K_CORRUPT_POS = 8, K_PAYLOAD = 9 };

#define CHECKSUM_HEADER_BYTES 11U /* kind(1) sequence(4) ack(4) length(2) */

/* ---------------------------------------------------------------------------------------------
 * Deterministic randomness: a stateless hash of (seed, sequence, attempt, purpose, extra).
 * ------------------------------------------------------------------------------------------- */
static uint32_t mix32(uint32_t value) {
    value ^= value >> 16U; value *= 0x7feb352dU; value ^= value >> 15U;
    value *= 0x846ca68bU; value ^= value >> 16U;
    return value;
}

static uint32_t draw(const NrArqSimulation *s, unsigned sequence, unsigned attempt, unsigned kind, unsigned extra) {
    uint32_t value = (uint32_t)s->config.seed ^ (sequence + 1U) * 0x9e3779b9U ^ (attempt + 1U) * 0x85ebca6bU ^ (kind + 1U) * 0xc2b2ae35U;
    if (extra != 0U) value ^= mix32(extra * 0x27d4eb2fU + kind);
    return mix32(value);
}

static bool chance(const NrArqSimulation *s, double probability, unsigned sequence, unsigned attempt, unsigned kind, unsigned extra) {
    if (!(probability > 0.0)) return false;
    if (probability >= 1.0) return true;
    return (double)(draw(s, sequence, attempt, kind, extra) % 1000000U) / 1000000.0 < probability;
}

/* ---------------------------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------------------------- */
const char *nr_arq_protocol_name(NrArqProtocol p) { static const char *names[] = {"Stop-and-Wait", "Go-Back-N", "Selective Repeat"}; return p >= NR_ARQ_STOP_WAIT && p < NR_ARQ_PROTOCOL_COUNT ? names[p] : "Unknown"; }
const char *nr_arq_state_name(NrArqState st) { static const char *names[] = {"Idle", "Running", "Paused", "Complete", "Failed", "Stopped"}; return st >= NR_ARQ_IDLE && st <= NR_ARQ_STOPPED ? names[st] : "Unknown"; }
const char *nr_arq_stop_reason_name(NrArqStopReason r) { static const char *names[] = {"none", "complete", "retry_limit", "time_limit", "capacity", "user"}; return r >= NR_STOP_NONE && r <= NR_STOP_USER ? names[r] : "unknown"; }
const char *nr_fault_type_name(NrFaultType t) { static const char *names[] = {"data_loss", "data_corruption", "ack_loss", "ack_corruption", "queue_overflow", "link_outage", "no_route"}; return t >= NR_FAULT_DATA_LOSS && t < NR_FAULT_TYPE_COUNT ? names[t] : "unknown"; }
const char *nr_fault_source_name(NrFaultSource src) { static const char *names[] = {"random", "link_probability", "scenario", "congestion", "topology"}; return src >= NR_SRC_RANDOM && src < NR_SRC_COUNT ? names[src] : "unknown"; }
const char *nr_fault_response_name(NrFaultResponse r) { static const char *names[] = {"pending", "nak_sent", "discarded_await_timeout", "retransmitted_after_timeout", "retransmitted_after_nak", "retransmitted_go_back", "covered_by_later_ack", "no_impact", "undetected"}; return r >= NR_RESP_PENDING && r < NR_RESP_COUNT ? names[r] : "unknown"; }
const char *nr_fault_rule_name(NrFaultRuleType t) { static const char *names[] = {"drop_data", "corrupt_data", "drop_ack", "corrupt_ack", "burst_loss", "link_outage", "queue_limit"}; return t >= NR_RULE_DROP_DATA && t < NR_RULE_TYPE_COUNT ? names[t] : "unknown"; }
const char *nr_congestion_mode_name(NrCongestionMode m) { static const char *names[] = {"none", "rate_aimd", "window_aimd"}; return m >= NR_CC_NONE && m < NR_CC_MODE_COUNT ? names[m] : "unknown"; }
const char *nr_retx_cause_name(NrRetxCause c) { static const char *names[] = {"none", "timeout", "nak", "go_back"}; return c >= NR_CAUSE_NONE && c <= NR_CAUSE_GOBACK ? names[c] : "unknown"; }
const char *nr_arq_trace_name(NrArqTraceType t) {
    static const char *names[] = {
        "DATA_SEND", "DATA_RETX", "DATA_ACCEPTED", "DATA_BUFFERED", "DATA_DELIVERED", "DATA_DUPLICATE",
        "DATA_DISCARDED_OUT_OF_ORDER", "DATA_OUT_OF_WINDOW", "DATA_CORRUPT_DETECTED", "DATA_UNDETECTED_CORRUPTION",
        "DATA_LOST", "ACK_SEND", "NAK_SEND", "ACK_RECEIVED", "ACK_DUPLICATE", "NAK_RECEIVED", "ACK_LOST",
        "ACK_CORRUPT_DETECTED", "TIMEOUT", "CORRUPTION_INJECTED", "CONGESTION_DECREASE", "CONGESTION_INCREASE",
        "LINK_DOWN", "LINK_UP", "REROUTE", "COMPLETE", "FAILED", "TIME_LIMIT", "PAUSED", "RESUMED", "STOPPED", "STARTED"};
    return t >= NR_TR_DATA_SEND && t < NR_TR_TYPE_COUNT ? names[t] : "UNKNOWN";
}

void nr_arq_default_config(NrArqConfig *c) {
    if (c == NULL) return;
    memset(c, 0, sizeof(*c));
    c->protocol = NR_ARQ_GO_BACK_N; c->packet_count = 20U; c->window_size = 4U; c->payload_bytes = 512U;
    c->packets_per_second = 100.0; c->timeout_seconds = 0.25; c->seed = 424242U; c->max_retries = 15U;
    c->use_nak = true; c->corruption_bits = 1U; c->cross_traffic_bytes = 1000U;
}

bool nr_scenario_add(NrFaultScenario *scenario, const NrFaultRule *rule) {
    if (scenario == NULL || rule == NULL || scenario->count >= NR_ARQ_MAX_RULES || rule->type < NR_RULE_DROP_DATA || rule->type >= NR_RULE_TYPE_COUNT) return false;
    if (rule->type == NR_RULE_LINK_OUTAGE && (!isfinite(rule->start) || !isfinite(rule->end) || rule->start < 0.0 || rule->end <= rule->start)) return false;
    scenario->rules[scenario->count++] = *rule;
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Text log and structured trace
 * ------------------------------------------------------------------------------------------- */
void nr_arq_log(NrArqSimulation *s, const char *message) {
    if (s == NULL || message == NULL) return;
    (void)snprintf(s->log[s->log_cursor], NR_EVENT_LEN, "[%07.3fs] %.110s", s->time_seconds, message);
    s->log_cursor = (s->log_cursor + 1U) % NR_ARQ_LOG;
    if (s->log_count < NR_ARQ_LOG) ++s->log_count;
}

static const char *node_name(const NrArqSimulation *s, int id) {
    const NrNode *node = nr_find_node((NrNetwork *)s->network, id);
    return node == NULL ? "?" : node->name;
}

static const char *loss_reason(int fault_type) {
    switch (fault_type) {
    case NR_FAULT_QUEUE_OVERFLOW: return "queue overflow";
    case NR_FAULT_LINK_OUTAGE: return "link outage";
    case NR_FAULT_NO_ROUTE: return "no route";
    default: return "lost in transit";
    }
}

void nr_arq_trace_describe(const NrArqSimulation *s, const NrArqTrace *r, char *b, size_t n) {
    if (s == NULL || r == NULL || b == NULL || n == 0U) return;
    const char *src_name = nr_fault_source_name((NrFaultSource)r->source);
    switch (r->type) {
    case NR_TR_DATA_SEND: (void)snprintf(b, n, "Sender transmits DATA seq %d", r->sequence); break;
    case NR_TR_DATA_RETX: (void)snprintf(b, n, "Sender retransmits DATA seq %d (attempt %d, cause: %s)", r->sequence, r->attempt, nr_retx_cause_name((NrRetxCause)r->detail)); break;
    case NR_TR_DATA_ACCEPTED: (void)snprintf(b, n, "Receiver accepts DATA seq %d (checksum OK, in order)", r->sequence); break;
    case NR_TR_DATA_BUFFERED: (void)snprintf(b, n, "Receiver buffers out-of-order DATA seq %d (waiting for seq %d)", r->sequence, r->ack); break;
    case NR_TR_DATA_DELIVERED: (void)snprintf(b, n, "DATA seq %d delivered to the application", r->sequence); break;
    case NR_TR_DATA_DUPLICATE: (void)snprintf(b, n, "Duplicate DATA seq %d discarded and re-acknowledged (no duplicate delivery)", r->sequence); break;
    case NR_TR_DATA_DISCARDED_OUT_OF_ORDER: (void)snprintf(b, n, "Receiver discards out-of-order DATA seq %d (expects seq %d)", r->sequence, r->ack); break;
    case NR_TR_DATA_OUT_OF_WINDOW: (void)snprintf(b, n, "DATA seq %d outside the receive window; discarded", r->sequence); break;
    case NR_TR_DATA_CORRUPT_DETECTED: (void)snprintf(b, n, "Checksum mismatch on DATA seq %d (attempt %d): frame rejected%s", r->sequence, r->attempt, r->detail ? ", NAK sent" : ", waiting for timeout"); break;
    case NR_TR_DATA_UNDETECTED_CORRUPTION: (void)snprintf(b, n, "Corrupted DATA seq %d passed the checksum (undetected error)", r->sequence); break;
    case NR_TR_DATA_LOST: (void)snprintf(b, n, "DATA seq %d (attempt %d) %s at %s (%s)", r->sequence, r->attempt, loss_reason(r->detail), node_name(s, r->node_id), src_name); break;
    case NR_TR_ACK_SEND: (void)snprintf(b, n, "Receiver sends ACK %d (response to seq %d)", r->ack, r->sequence); break;
    case NR_TR_NAK_SEND: (void)snprintf(b, n, "Receiver sends NAK for seq %d", r->sequence); break;
    case NR_TR_ACK_RECEIVED: (void)snprintf(b, n, "Sender receives ACK %d (response to seq %d)", r->ack, r->sequence); break;
    case NR_TR_ACK_DUPLICATE: (void)snprintf(b, n, "Sender ignores duplicate ACK %d", r->ack); break;
    case NR_TR_NAK_RECEIVED: (void)snprintf(b, n, "Sender receives NAK for seq %d", r->sequence); break;
    case NR_TR_ACK_LOST: (void)snprintf(b, n, "%s for seq %d %s at %s (%s)", r->detail == -1 ? "NAK" : "ACK", r->sequence, loss_reason(r->detail), node_name(s, r->node_id), src_name); break;
    case NR_TR_ACK_CORRUPT_DETECTED: (void)snprintf(b, n, "Corrupted ACK/NAK for seq %d discarded by sender (checksum mismatch)", r->sequence); break;
    case NR_TR_TIMEOUT: (void)snprintf(b, n, "Retransmission timer expired for seq %d", r->sequence); break;
    case NR_TR_CORRUPTION_INJECTED: (void)snprintf(b, n, "Bit errors injected into %s seq %d on link %d (%s)", r->detail == NR_ARQ_DATA_EVENT ? "DATA" : "ACK/NAK", r->sequence, r->link_id, src_name); break;
    case NR_TR_CONGESTION_DECREASE: (void)snprintf(b, n, "Congestion: sending %s reduced to %.2f", r->source == 1 ? "window" : "rate", (double)r->detail / 100.0); break;
    case NR_TR_CONGESTION_INCREASE: (void)snprintf(b, n, "Congestion cleared: sending rate increased to %.2f", (double)r->detail / 100.0); break;
    case NR_TR_LINK_DOWN: (void)snprintf(b, n, "Link %d DOWN (scheduled outage)", r->link_id); break;
    case NR_TR_LINK_UP: (void)snprintf(b, n, "Link %d restored", r->link_id); break;
    case NR_TR_REROUTE: (void)snprintf(b, n, "Topology changed; in-flight data and ACK events rerouted on the active graph"); break;
    case NR_TR_COMPLETE: (void)snprintf(b, n, "All packets acknowledged; transfer complete"); break;
    case NR_TR_FAILED: (void)snprintf(b, n, "Transfer aborted: %s", r->detail == NR_STOP_RETRY_LIMIT ? "retry limit reached" : "engine capacity reached"); break;
    case NR_TR_TIME_LIMIT: (void)snprintf(b, n, "Transfer stopped at the simulation time limit (incomplete)"); break;
    case NR_TR_PAUSED: (void)snprintf(b, n, "Traffic paused"); break;
    case NR_TR_RESUMED: (void)snprintf(b, n, "Traffic resumed"); break;
    case NR_TR_STOPPED: (void)snprintf(b, n, "Transfer stopped by the user"); break;
    case NR_TR_STARTED: (void)snprintf(b, n, "Reliable transfer started (%s, %zu packets)", nr_arq_protocol_name(s->config.protocol), s->config.packet_count); break;
    default: (void)snprintf(b, n, "Event"); break;
    }
}

static void trace_add(NrArqSimulation *s, NrArqTraceType type, uint32_t frame_id, int sequence, int ack, int attempt,
                      int node_id, int link_id, int detail, int source) {
    if (!s->recording) return;
    NrArqTrace record = {s->time_seconds, type, frame_id, sequence, ack, attempt, node_id, link_id, detail, source};
    if (s->trace_count < NR_ARQ_TRACE_LIMIT) {
        if (s->trace_count == s->trace_capacity) {
            const size_t capacity = s->trace_capacity == 0U ? 1024U : s->trace_capacity * 2U;
            NrArqTrace *grown = realloc(s->trace, capacity * sizeof(*grown));
            if (grown != NULL) { s->trace = grown; s->trace_capacity = capacity; }
        }
        if (s->trace_count < s->trace_capacity) s->trace[s->trace_count++] = record; else ++s->trace_dropped;
    } else ++s->trace_dropped;
    char text[NR_EVENT_LEN];
    nr_arq_trace_describe(s, &record, text, sizeof(text));
    nr_arq_log(s, text);
}

/* ---------------------------------------------------------------------------------------------
 * Event heap (ordered by time, then by insertion order for determinism)
 * ------------------------------------------------------------------------------------------- */
static bool heap_before(const NrArqScheduled *a, const NrArqScheduled *b) {
    return a->time < b->time || (a->time == b->time && a->order < b->order);
}

static void fail_transfer(NrArqSimulation *s, NrArqStopReason reason);

static void schedule(NrArqSimulation *s, double time, unsigned type, uint32_t a, uint32_t b) {
    if (s->event_count == s->heap_capacity) {
        const size_t capacity = s->heap_capacity * 2U;
        NrArqScheduled *grown = capacity > 4000000U ? NULL : realloc(s->heap, capacity * sizeof(*grown));
        if (grown == NULL) { fail_transfer(s, NR_STOP_CAPACITY); return; }
        s->heap = grown; s->heap_capacity = capacity;
    }
    NrArqScheduled item = {time, s->event_order++, type, a, b};
    size_t i = s->event_count++;
    while (i > 0U) {
        const size_t parent = (i - 1U) / 2U;
        if (!heap_before(&item, &s->heap[parent])) break;
        s->heap[i] = s->heap[parent]; i = parent;
    }
    s->heap[i] = item;
}

static NrArqScheduled heap_pop(NrArqSimulation *s) {
    NrArqScheduled top = s->heap[0];
    const NrArqScheduled last = s->heap[--s->event_count];
    size_t i = 0U;
    for (;;) {
        size_t child = 2U * i + 1U;
        if (child >= s->event_count) break;
        if (child + 1U < s->event_count && heap_before(&s->heap[child + 1U], &s->heap[child])) ++child;
        if (!heap_before(&s->heap[child], &last)) break;
        s->heap[i] = s->heap[child]; i = child;
    }
    if (s->event_count > 0U) s->heap[i] = last;
    return top;
}

/* ---------------------------------------------------------------------------------------------
 * Wire-frame pool
 * ------------------------------------------------------------------------------------------- */
static int wire_alloc(NrArqSimulation *s) {
    if (s->pool_free_count == 0U) {
        const size_t old = s->pool_capacity, capacity = old * 2U;
        NrArqWire *pool = capacity > 1000000U ? NULL : realloc(s->pool, capacity * sizeof(*pool));
        if (pool == NULL) return -1;
        s->pool = pool;
        uint32_t *free_list = realloc(s->pool_free, capacity * sizeof(*free_list));
        if (free_list == NULL) return -1;
        s->pool_free = free_list;
        for (size_t i = capacity; i > old; --i) { s->pool[i - 1U].state = NR_WIRE_FREE; s->pool_free[s->pool_free_count++] = (uint32_t)(i - 1U); }
        s->pool_capacity = capacity;
    }
    const int index = (int)s->pool_free[--s->pool_free_count];
    NrArqWire *w = &s->pool[index];
    memset(w, 0, sizeof(*w));
    w->id = ++s->next_frame_id; w->state = NR_WIRE_QUEUED; w->loss_hop = -1; w->corrupt_hop = -1;
    w->link_index = -1; w->next_node = -1;
    return index;
}

static void wire_release(NrArqSimulation *s, int index) {
    s->pool[index].state = NR_WIRE_FREE;
    s->pool_free[s->pool_free_count++] = (uint32_t)index;
}

/* ---------------------------------------------------------------------------------------------
 * Checksums: FNV-1a over the frame's header fields and deterministic payload bytes
 * ------------------------------------------------------------------------------------------- */
static uint32_t wire_checksum(NrArqSimulation *s, const NrArqWire *w, bool as_received) {
    const size_t payload = w->kind == NR_ARQ_DATA_EVENT ? s->config.payload_bytes : 0U;
    const size_t length = CHECKSUM_HEADER_BYTES + payload;
    unsigned char *b = s->scratch;
    b[0] = (unsigned char)w->kind;
    for (unsigned i = 0; i < 4U; ++i) { b[1U + i] = (unsigned char)(w->sequence >> (8U * i)); b[5U + i] = (unsigned char)(w->ack >> (8U * i)); }
    b[9] = (unsigned char)(payload & 0xFFU); b[10] = (unsigned char)((payload >> 8U) & 0xFFU);
    if (payload > 0U) {
        uint32_t x = draw(s, w->sequence, 0U, K_PAYLOAD, 0U) | 1U;
        for (size_t i = 0; i < payload; ++i) { x ^= x << 13U; x ^= x >> 17U; x ^= x << 5U; b[CHECKSUM_HEADER_BYTES + i] = (unsigned char)x; }
    }
    if (as_received && w->corrupted) {
        /* Burst error of `corruption_bits` consecutive bits. Data corruption hits the payload (the
         * header is assumed protected by the link layer); ACK corruption hits the sequence/ack fields. */
        const size_t start = payload > 0U ? CHECKSUM_HEADER_BYTES : 1U;
        const size_t region_bits = (payload > 0U ? payload : 8U) * 8U;
        unsigned bits = s->config.corruption_bits == 0U ? 1U : s->config.corruption_bits;
        if (bits > region_bits) bits = (unsigned)region_bits;
        const size_t first = mix32(w->corrupt_key) % region_bits;
        for (unsigned k = 0; k < bits; ++k) {
            const size_t bit = (first + k) % region_bits;
            b[start + bit / 8U] ^= (unsigned char)(1U << (bit % 8U));
        }
    }
    return nr_checksum(b, length);
}

/* ---------------------------------------------------------------------------------------------
 * Fault ledger
 * ------------------------------------------------------------------------------------------- */
static int record_fault(NrArqSimulation *s, NrFaultType type, NrFaultSource source, const NrArqWire *w, int node_id, int link_id) {
    if (s->fault_count >= NR_ARQ_FAULT_LIMIT) return -1;
    if (s->fault_count == s->fault_capacity) {
        const size_t capacity = s->fault_capacity == 0U ? 256U : s->fault_capacity * 2U;
        NrArqFault *grown = realloc(s->faults, capacity * sizeof(*grown));
        if (grown == NULL) return -1;
        s->faults = grown; s->fault_capacity = capacity;
    }
    const int index = (int)s->fault_count++;
    NrArqFault *f = &s->faults[index];
    memset(f, 0, sizeof(*f));
    f->time = s->time_seconds; f->type = type; f->source = source; f->frame_kind = w->kind; f->frame_id = w->id;
    f->sequence = w->sequence; f->attempt = w->attempt; f->node_id = node_id; f->link_id = link_id;
    f->response = NR_RESP_PENDING; f->next_same_sequence = -1;
    if (w->sequence < s->config.packet_count) {
        NrArqFrame *frame = &s->frames[w->sequence];
        f->next_same_sequence = frame->fault_head; frame->fault_head = index;
        const bool is_data = w->kind == NR_ARQ_DATA_EVENT;
        const bool loss = type != NR_FAULT_DATA_CORRUPTION && type != NR_FAULT_ACK_CORRUPTION;
        /* A lost copy of data the receiver already has (or of an ACK for data already acknowledged) cannot hurt. */
        if (loss && ((is_data && frame->received) || (!is_data && frame->acknowledged))) {
            f->response = NR_RESP_NO_IMPACT; f->recovered = true; f->recovered_at = s->time_seconds;
        }
    }
    ++s->fault_version;
    return index;
}

static NrArqFault *fault_for_frame(NrArqSimulation *s, const NrArqWire *w) {
    if (w->sequence >= s->config.packet_count) return NULL;
    for (int i = s->frames[w->sequence].fault_head; i >= 0; i = s->faults[i].next_same_sequence)
        if (s->faults[i].frame_id == w->id) return &s->faults[i];
    return NULL;
}

static void faults_on_retransmit(NrArqSimulation *s, size_t seq, NrRetxCause cause) {
    for (int i = s->frames[seq].fault_head; i >= 0; i = s->faults[i].next_same_sequence) {
        NrArqFault *f = &s->faults[i];
        if (f->recovered || !(f->response == NR_RESP_PENDING || f->response == NR_RESP_NAK_SENT || f->response == NR_RESP_DISCARDED)) continue;
        if (!f->detected) { f->detected = true; f->detected_at = s->time_seconds; }
        f->response = cause == NR_CAUSE_NAK ? NR_RESP_RETX_NAK : cause == NR_CAUSE_GOBACK ? NR_RESP_RETX_GOBACK : NR_RESP_RETX_TIMEOUT;
        ++s->fault_version;
    }
}

static void faults_on_received(NrArqSimulation *s, size_t seq) {
    for (int i = s->frames[seq].fault_head; i >= 0; i = s->faults[i].next_same_sequence) {
        NrArqFault *f = &s->faults[i];
        if (f->recovered || f->frame_kind != NR_ARQ_DATA_EVENT) continue;
        f->recovered = true; f->recovered_at = s->time_seconds; ++s->fault_version;
    }
}

static void faults_on_acknowledged(NrArqSimulation *s, size_t seq) {
    for (int i = s->frames[seq].fault_head; i >= 0; i = s->faults[i].next_same_sequence) {
        NrArqFault *f = &s->faults[i];
        if (f->recovered) continue;
        if (f->response == NR_RESP_PENDING) f->response = NR_RESP_COVERED_BY_ACK;
        f->recovered = true; f->recovered_at = s->time_seconds; ++s->fault_version;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Topology helpers
 * ------------------------------------------------------------------------------------------- */
static int node_index(const NrNetwork *n, int id) {
    for (size_t i = 0; i < n->node_count; ++i) if (n->nodes[i].id == id) return (int)i;
    return -1;
}

static bool node_active(const NrArqSimulation *s, int id) {
    const int index = node_index(s->network, id);
    return index >= 0 && s->network->nodes[index].active;
}

static bool link_usable(const NrArqSimulation *s, int link_index) {
    return link_index >= 0 && (size_t)link_index < s->network->link_count && s->network->links[link_index].active && !s->link_blocked[link_index];
}

static uint64_t topology_signature(const NrNetwork *n) {
    uint64_t h = 1469598103934665603ULL ^ ((uint64_t)n->node_count << 32U) ^ n->link_count;
    for (size_t i = 0; i < n->node_count; ++i) { h ^= (uint64_t)(n->nodes[i].id * 2 + (n->nodes[i].active ? 1 : 0)); h *= 1099511628211ULL; }
    for (size_t i = 0; i < n->link_count; ++i) { h ^= (uint64_t)(n->links[i].id * 2 + (n->links[i].active ? 1 : 0)); h *= 1099511628211ULL; }
    return h;
}

static void recompute_routes(NrArqSimulation *s) {
    memset(s->next_hop_valid, 0, sizeof(s->next_hop_valid));
    s->forward_route = nr_find_route_masked(s->network, s->source, s->destination, s->link_blocked);
    s->reverse_route = nr_find_route_masked(s->network, s->destination, s->source, s->link_blocked);
}

/* Applies a change of active flags / outages: links that just went down drop the frames propagating on them. */
static void apply_topology_change(NrArqSimulation *s) {
    for (size_t j = 0; j < s->network->link_count && j < NR_MAX_LINKS; ++j) {
        const bool usable = link_usable(s, (int)j);
        if (s->link_was_active[j] && !usable) s->link_down_at[j] = s->time_seconds;
        s->link_was_active[j] = usable;
    }
    s->topology_signature = topology_signature(s->network);
    recompute_routes(s);
    trace_add(s, NR_TR_REROUTE, 0U, -1, -1, 0, -1, -1, 0, NR_SRC_TOPOLOGY);
}

void nr_arq_topology_changed(NrArqSimulation *s) { if (s != NULL && s->network != NULL) apply_topology_change(s); }

static void check_topology(NrArqSimulation *s) {
    if (topology_signature(s->network) != s->topology_signature) apply_topology_change(s);
}

/* Next hop from node_id toward target (cached per direction for the transfer's two endpoints). */
static int next_hop(NrArqSimulation *s, int node_id, int target, int *link_index) {
    const int direction = target == s->destination ? 0 : target == s->source ? 1 : -1;
    const int index = node_index(s->network, node_id);
    if (index < 0) return -1;
    if (direction >= 0 && s->next_hop_valid[direction][index]) {
        *link_index = s->next_hop_link[direction][index];
        return s->next_hop_node[direction][index];
    }
    const NrRoute route = nr_find_route_masked(s->network, node_id, target, s->link_blocked);
    int next = -1, link = -1;
    if (route.reachable && route.length >= 2U) {
        next = route.nodes[1];
        link = nr_find_link_between(s->network, node_id, next, s->link_blocked);
        if (link < 0) next = -1;
    }
    if (direction >= 0) {
        s->next_hop_valid[direction][index] = true;
        s->next_hop_node[direction][index] = next; s->next_hop_link[direction][index] = link;
    }
    *link_index = link;
    return next;
}

unsigned nr_arq_port_capacity(const NrArqSimulation *s, size_t port_index) {
    if (s == NULL || port_index >= s->port_count || port_index / 2U >= s->network->link_count) return 0U;
    const NrLink *link = &s->network->links[port_index / 2U];
    const int owner = port_index % 2U == 0U ? link->from : link->to;
    if (owner == s->source || owner == s->destination) return NR_ARQ_MAX_QUEUE; /* endpoint's own transmit buffer */
    const int index = node_index(s->network, owner);
    if (index < 0) return 0U;
    unsigned capacity = s->queue_override[index] > 0U ? s->queue_override[index] : s->network->nodes[index].queue_capacity;
    return capacity > NR_ARQ_MAX_QUEUE ? NR_ARQ_MAX_QUEUE : capacity;
}

static void port_account(NrArqSimulation *s, NrArqPort *p) {
    p->occupancy_area += (double)p->count * (s->time_seconds - p->last_change);
    p->last_change = s->time_seconds;
}

/* ---------------------------------------------------------------------------------------------
 * Congestion control
 * ------------------------------------------------------------------------------------------- */
unsigned nr_arq_effective_window(const NrArqSimulation *s) {
    if (s == NULL) return 0U;
    if (s->config.protocol == NR_ARQ_STOP_WAIT) return 1U;
    unsigned window = s->config.window_size;
    if (s->config.congestion_mode == NR_CC_WINDOW_AIMD) {
        const double cwnd = floor(s->cwnd);
        if (cwnd < (double)window) window = cwnd < 1.0 ? 1U : (unsigned)cwnd;
    }
    return window;
}

static double sending_rate(const NrArqSimulation *s) {
    return s->config.congestion_mode == NR_CC_RATE_AIMD ? s->congestion.rate : s->config.packets_per_second;
}

static void note_rate_change(NrArqSimulation *s, double before, unsigned decreases_before, unsigned increases_before) {
    if (s->congestion.decreases != decreases_before) {
        s->stats.congestion_decreases += s->congestion.decreases - decreases_before;
        s->last_window_decrease = s->time_seconds;
        trace_add(s, NR_TR_CONGESTION_DECREASE, 0U, -1, -1, 0, -1, -1, (int)lround(s->congestion.rate * 100.0), 0);
    }
    if (s->congestion.increases != increases_before) {
        s->stats.congestion_increases += s->congestion.increases - increases_before;
        trace_add(s, NR_TR_CONGESTION_INCREASE, 0U, -1, -1, 0, -1, -1, (int)lround(s->congestion.rate * 100.0), 0);
    }
    (void)before;
    if (s->congestion.rate < s->stats.min_rate) s->stats.min_rate = s->congestion.rate;
}

/* Explicit congestion signal (queue overflow on the path), at most one decrease per timeout interval. */
static void congestion_signal(NrArqSimulation *s) {
    if (s->config.congestion_mode != NR_CC_RATE_AIMD) return;
    if (s->stats.congestion_decreases > 0U && s->time_seconds - s->last_window_decrease < s->config.timeout_seconds) return;
    const double before = s->congestion.rate; const unsigned d = s->congestion.decreases, i = s->congestion.increases;
    (void)nr_congestion_observe(&s->congestion, NR_ARQ_MONITOR_INTERVAL, 1.0, true);
    note_rate_change(s, before, d, i);
}

static void window_on_ack(NrArqSimulation *s) {
    if (s->config.congestion_mode != NR_CC_WINDOW_AIMD) return;
    if (s->cwnd < s->ssthresh) s->cwnd += 1.0;            /* slow start */
    else s->cwnd += 1.0 / s->cwnd;                         /* additive increase */
    if (s->cwnd > (double)s->config.window_size) s->cwnd = (double)s->config.window_size;
}

static void window_on_timeout(NrArqSimulation *s) {
    if (s->config.congestion_mode != NR_CC_WINDOW_AIMD) return;
    if (s->stats.congestion_decreases > 0U && s->time_seconds - s->last_window_decrease < s->config.timeout_seconds) return;
    s->ssthresh = fmax(1.0, s->cwnd / 2.0);
    s->cwnd = s->ssthresh;                                 /* multiplicative decrease */
    s->last_window_decrease = s->time_seconds;
    ++s->stats.congestion_decreases;
    if (s->cwnd < s->stats.min_window) s->stats.min_window = s->cwnd;
    trace_add(s, NR_TR_CONGESTION_DECREASE, 0U, -1, -1, 0, -1, -1, (int)lround(s->cwnd * 100.0), 1);
}

/* ---------------------------------------------------------------------------------------------
 * Metrics sampling
 * ------------------------------------------------------------------------------------------- */
static void sample_metrics(NrArqSimulation *s) {
    if (s->history_count == NR_ARQ_HISTORY) {
        for (size_t i = 0; i < NR_ARQ_HISTORY / 2U; ++i) s->history[i] = s->history[2U * i + 1U];
        s->history_count = NR_ARQ_HISTORY / 2U;
        s->history_interval *= 2.0;
    }
    NrArqSample *sample = &s->history[s->history_count++];
    memset(sample, 0, sizeof(*sample));
    sample->time_seconds = s->time_seconds;
    sample->delivery_ratio = s->config.packet_count == 0U ? 0.0 : (double)s->stats.delivered / (double)s->config.packet_count;
    sample->throughput_mbps = nr_arq_throughput_mbps(s);
    sample->average_latency_ms = s->stats.delivered == 0U ? 0.0 : s->stats.total_latency_ms / (double)s->stats.delivered;
    sample->queue_utilization = s->queue_utilization;
    sample->packet_loss_rate = s->stats.data_transmissions == 0U ? 0.0 : (double)s->stats.packet_losses / (double)s->stats.data_transmissions;
    sample->window = (double)nr_arq_effective_window(s);
    sample->rate = sending_rate(s);
    sample->retransmissions = s->stats.retransmissions;
    sample->delivered = s->stats.delivered;
    sample->in_flight = s->next_sequence - s->sender_base;
    s->next_sample_at = s->time_seconds + s->history_interval;
    ++s->history_version;
}

static void route_queue_utilization(NrArqSimulation *s) {
    double worst = 0.0;
    for (size_t i = 1U; i + 1U < s->forward_route.length; ++i) {
        const int link = nr_find_link_between(s->network, s->forward_route.nodes[i], s->forward_route.nodes[i + 1U], s->link_blocked);
        if (link < 0) continue;
        const size_t port = (size_t)link * 2U + (s->network->links[link].from == s->forward_route.nodes[i] ? 0U : 1U);
        const unsigned capacity = nr_arq_port_capacity(s, port);
        const NrArqPort *p = &s->ports[port];
        const double utilization = capacity == 0U ? (p->transmitting >= 0 ? 1.0 : 0.0) : (double)p->count / (double)capacity;
        if (utilization > worst) worst = utilization;
    }
    s->queue_utilization = fmin(1.0, worst);
}

/* ---------------------------------------------------------------------------------------------
 * Terminal states
 * ------------------------------------------------------------------------------------------- */
static void fail_transfer(NrArqSimulation *s, NrArqStopReason reason) {
    if (s->state != NR_ARQ_RUNNING && s->state != NR_ARQ_PAUSED) return;
    s->state = NR_ARQ_FAILED; s->stop_reason = reason;
    trace_add(s, reason == NR_STOP_TIME_LIMIT ? NR_TR_TIME_LIMIT : NR_TR_FAILED, 0U, -1, -1, 0, -1, -1, (int)reason, 0);
    sample_metrics(s);
}

static void complete_transfer(NrArqSimulation *s) {
    s->state = NR_ARQ_COMPLETE; s->stop_reason = NR_STOP_COMPLETE;
    trace_add(s, NR_TR_COMPLETE, 0U, -1, -1, 0, -1, -1, 0, 0);
    sample_metrics(s);
}

/* ---------------------------------------------------------------------------------------------
 * Frame movement: queue -> serialise -> propagate -> arrive
 * ------------------------------------------------------------------------------------------- */
static void receiver_handle(NrArqSimulation *s, const NrArqWire *w);
static void sender_handle(NrArqSimulation *s, const NrArqWire *w);

static void lose_wire(NrArqSimulation *s, int index, NrFaultType type, NrFaultSource source, int node_id, int link_index) {
    const NrArqWire w = s->pool[index];
    wire_release(s, index);
    const int link_id = link_index >= 0 && (size_t)link_index < s->network->link_count ? s->network->links[link_index].id : -1;
    if (w.kind == NR_ARQ_CROSS_EVENT) { if (type == NR_FAULT_QUEUE_OVERFLOW) ++s->stats.cross_traffic_drops; return; }
    const bool data = w.kind == NR_ARQ_DATA_EVENT;
    if (type == NR_FAULT_DATA_LOSS && !data) type = NR_FAULT_ACK_LOSS;
    if (data) ++s->stats.packet_losses; else ++s->stats.ack_losses;
    if (type == NR_FAULT_QUEUE_OVERFLOW) { ++s->stats.queue_drops; if (data) ++s->stats.data_queue_drops; else ++s->stats.ack_queue_drops; }
    (void)record_fault(s, type, source, &w, node_id, link_id);
    trace_add(s, data ? NR_TR_DATA_LOST : NR_TR_ACK_LOST, w.id, (int)w.sequence, (int)w.ack, (int)w.attempt, node_id, link_id,
              (!data && w.kind == NR_ARQ_NAK_EVENT && type == NR_FAULT_ACK_LOSS) ? -1 : (int)type, (int)source);
}

static void start_transmission(NrArqSimulation *s, size_t port_index, int index) {
    NrArqPort *p = &s->ports[port_index];
    NrArqWire *w = &s->pool[index];
    const NrLink *link = &s->network->links[port_index / 2U];
    p->transmitting = index;
    w->state = NR_WIRE_TRANSMITTING;
    const double tx = link->bandwidth_mbps > 0.0 ? (double)w->bytes * 8.0 / (link->bandwidth_mbps * 1000000.0) : 0.0;
    w->hop_start = s->time_seconds; w->hop_end = s->time_seconds + tx;
    schedule(s, s->time_seconds + tx, EV_TX_DONE, (uint32_t)port_index, w->id);
}

static void forward_wire(NrArqSimulation *s, int index, int node_id) {
    NrArqWire *w = &s->pool[index];
    w->at_node = node_id;
    int link_index = -1, next;
    if (w->kind == NR_ARQ_CROSS_EVENT) { next = w->next_node; link_index = w->link_index; if (!link_usable(s, link_index)) next = -1; }
    else next = next_hop(s, node_id, w->target, &link_index);
    if (next < 0 || link_index < 0) { lose_wire(s, index, NR_FAULT_NO_ROUTE, NR_SRC_TOPOLOGY, node_id, -1); return; }
    const size_t port_index = (size_t)link_index * 2U + (s->network->links[link_index].from == node_id ? 0U : 1U);
    NrArqPort *p = &s->ports[port_index];
    w->next_node = next; w->link_index = link_index;
    if (p->transmitting < 0) { start_transmission(s, port_index, index); return; }
    const unsigned capacity = nr_arq_port_capacity(s, port_index);
    if (p->count >= capacity) {
        ++p->drops;
        const bool cross = w->kind == NR_ARQ_CROSS_EVENT;
        lose_wire(s, index, NR_FAULT_QUEUE_OVERFLOW, NR_SRC_CONGESTION, node_id, link_index);
        s->queue_utilization = 1.0;
        if (!cross) congestion_signal(s);
        return;
    }
    if (p->queue == NULL) {
        p->queue = malloc(NR_ARQ_MAX_QUEUE * sizeof(*p->queue));
        if (p->queue == NULL) { lose_wire(s, index, NR_FAULT_QUEUE_OVERFLOW, NR_SRC_CONGESTION, node_id, link_index); return; }
    }
    port_account(s, p);
    p->queue[(p->head + p->count) % NR_ARQ_MAX_QUEUE] = (uint32_t)index;
    ++p->count;
    if (p->count > p->max_occupancy) p->max_occupancy = p->count;
    w->state = NR_WIRE_QUEUED;
}

static void on_tx_done(NrArqSimulation *s, size_t port_index, uint32_t frame_id) {
    NrArqPort *p = &s->ports[port_index];
    const int index = p->transmitting;
    if (index < 0 || s->pool[index].id != frame_id) return;
    p->transmitting = -1; ++p->forwarded;
    NrArqWire *w = &s->pool[index];
    if (!link_usable(s, w->link_index) || !node_active(s, w->next_node)) {
        lose_wire(s, index, NR_FAULT_LINK_OUTAGE, NR_SRC_TOPOLOGY, w->at_node, w->link_index);
    } else {
        const NrLink *link = &s->network->links[w->link_index];
        double delay = link->delay_ms / 1000.0;
        if (link->jitter_ms > 0.0) {
            const uint32_t u = draw(s, w->sequence, w->attempt, K_JITTER, (unsigned)w->kind * 7919U + w->hops_done * 131U + (unsigned)w->link_index + 1U);
            delay += link->jitter_ms / 1000.0 * (double)(u % 10001U) / 10000.0;
        }
        w->state = NR_WIRE_PROPAGATING; w->hop_start = s->time_seconds; w->hop_end = s->time_seconds + delay;
        schedule(s, w->hop_end, EV_ARRIVE, (uint32_t)index, w->id);
    }
    if (p->count > 0U && s->state != NR_ARQ_FAILED) {
        port_account(s, p);
        const int next = (int)p->queue[p->head];
        p->head = (p->head + 1U) % NR_ARQ_MAX_QUEUE; --p->count;
        start_transmission(s, port_index, next);
    }
}

static void on_arrive(NrArqSimulation *s, int index, uint32_t frame_id) {
    NrArqWire *w = &s->pool[index];
    if (w->state != NR_WIRE_PROPAGATING || w->id != frame_id) return;
    const unsigned hop = w->hops_done++;
    const int link_index = w->link_index, arriving = w->next_node;
    const NrLink *link = &s->network->links[link_index];
    if (!link_usable(s, link_index) || s->link_down_at[link_index] > w->hop_start) {
        lose_wire(s, index, NR_FAULT_LINK_OUTAGE, NR_SRC_TOPOLOGY, arriving, link_index); return;
    }
    if (w->kind == NR_ARQ_CROSS_EVENT) { wire_release(s, index); return; }
    const bool data = w->kind == NR_ARQ_DATA_EVENT;
    if (w->loss_hop == (int)hop) { lose_wire(s, index, data ? NR_FAULT_DATA_LOSS : NR_FAULT_ACK_LOSS, w->loss_source, arriving, link_index); return; }
    if (link->loss_probability > 0.0 &&
        chance(s, link->loss_probability, w->sequence, w->attempt, K_LINK_LOSS, (unsigned)w->kind * 4096U + (unsigned)link_index + 1U)) {
        lose_wire(s, index, data ? NR_FAULT_DATA_LOSS : NR_FAULT_ACK_LOSS, NR_SRC_LINK_PROBABILITY, arriving, link_index); return;
    }
    if (w->corrupt_hop == (int)hop && !w->corrupted) {
        w->corrupted = true;
        (void)record_fault(s, data ? NR_FAULT_DATA_CORRUPTION : NR_FAULT_ACK_CORRUPTION, w->corrupt_source, w, arriving, link->id);
        trace_add(s, NR_TR_CORRUPTION_INJECTED, w->id, (int)w->sequence, (int)w->ack, (int)w->attempt, arriving, link->id, (int)w->kind, (int)w->corrupt_source);
    }
    if (!node_active(s, arriving)) { lose_wire(s, index, NR_FAULT_LINK_OUTAGE, NR_SRC_TOPOLOGY, arriving, link_index); return; }
    if (arriving == w->target) {
        NrArqWire copy = *w;
        copy.at_node = arriving;
        wire_release(s, index);
        if (copy.kind == NR_ARQ_DATA_EVENT) receiver_handle(s, &copy); else sender_handle(s, &copy);
        return;
    }
    forward_wire(s, index, arriving);
}

/* ---------------------------------------------------------------------------------------------
 * Fault decisions at frame creation
 * ------------------------------------------------------------------------------------------- */
static void match_rules(const NrArqSimulation *s, bool data, unsigned seq, unsigned attempt, bool *drop, bool *corrupt) {
    *drop = false; *corrupt = false;
    for (size_t i = 0; i < s->scenario.count; ++i) {
        const NrFaultRule *r = &s->scenario.rules[i];
        const bool attempt_matches = r->attempt == 0U || r->attempt == attempt;
        if (!attempt_matches) continue;
        switch (r->type) {
        case NR_RULE_DROP_DATA: if (data && r->sequence == seq) *drop = true; break;
        case NR_RULE_CORRUPT_DATA: if (data && r->sequence == seq) *corrupt = true; break;
        case NR_RULE_DROP_ACK: if (!data && r->sequence == seq) *drop = true; break;
        case NR_RULE_CORRUPT_ACK: if (!data && r->sequence == seq) *corrupt = true; break;
        case NR_RULE_BURST_LOSS: if (data && seq >= r->sequence && seq < r->sequence + (r->count == 0U ? 1U : r->count)) *drop = true; break;
        default: break;
        }
    }
}

static void decide_fate(NrArqSimulation *s, NrArqWire *w, size_t hops) {
    const bool data = w->kind == NR_ARQ_DATA_EVENT;
    const double loss = data ? s->config.loss_probability : s->config.ack_loss_probability;
    const double corruption = data ? s->config.corruption_probability : s->config.ack_corruption_probability;
    const unsigned loss_kind = data ? K_DATA_LOSS : K_ACK_LOSS, corrupt_kind = data ? K_DATA_CORRUPT : K_ACK_CORRUPT;
    const unsigned salt = data ? 0U : 1U;
    if (hops == 0U) hops = 1U;
    bool rule_drop, rule_corrupt;
    match_rules(s, data, w->sequence, w->attempt, &rule_drop, &rule_corrupt);
    if (rule_drop) { w->loss_hop = (int)((hops - 1U) / 2U); w->loss_source = NR_SRC_SCENARIO; }
    else if (chance(s, loss, w->sequence, w->attempt, loss_kind, 0U)) {
        w->loss_hop = (int)(draw(s, w->sequence, w->attempt, K_LOSS_HOP, salt + 1U) % hops); w->loss_source = NR_SRC_RANDOM;
    }
    if (w->loss_hop >= 0) return;
    if (rule_corrupt) { w->corrupt_hop = (int)((hops - 1U) / 2U); w->corrupt_source = NR_SRC_SCENARIO; }
    else if (chance(s, corruption, w->sequence, w->attempt, corrupt_kind, 0U)) {
        w->corrupt_hop = (int)(draw(s, w->sequence, w->attempt, K_CORRUPT_HOP, salt + 1U) % hops); w->corrupt_source = NR_SRC_RANDOM;
    }
    w->corrupt_key = draw(s, w->sequence, w->attempt, K_CORRUPT_POS, salt + 1U);
}

/* ---------------------------------------------------------------------------------------------
 * Sender
 * ------------------------------------------------------------------------------------------- */
static void restart_gbn_timer(NrArqSimulation *s) {
    ++s->gbn_timer_token; s->gbn_timer_running = true;
    schedule(s, s->time_seconds + s->config.timeout_seconds, EV_GBN_TIMEOUT, 0U, s->gbn_timer_token);
}

static bool send_data(NrArqSimulation *s, size_t seq, NrRetxCause cause) {
    NrArqFrame *f = &s->frames[seq];
    if (f->acknowledged || f->failed || s->state != NR_ARQ_RUNNING) return false;
    const bool retransmission = f->attempts > 0U;
    if (retransmission) {
        if (f->attempts - 1U >= s->config.max_retries) {
            f->failed = true;
            fail_transfer(s, NR_STOP_RETRY_LIMIT);
            return false;
        }
        ++s->stats.retransmissions;
        if (cause == NR_CAUSE_NAK) ++s->stats.retx_nak; else if (cause == NR_CAUSE_GOBACK) ++s->stats.retx_goback; else ++s->stats.retx_timeout;
        faults_on_retransmit(s, seq, cause);
    } else {
        f->created_at = s->time_seconds; ++s->stats.generated;
    }
    ++f->attempts; f->last_sent_at = s->time_seconds;
    const int index = wire_alloc(s);
    if (index < 0) { fail_transfer(s, NR_STOP_CAPACITY); return false; }
    NrArqWire *w = &s->pool[index];
    w->kind = NR_ARQ_DATA_EVENT; w->sequence = (unsigned)seq; w->attempt = f->attempts;
    w->bytes = s->config.payload_bytes + NR_ARQ_HEADER_BYTES; w->origin = s->source; w->target = s->destination; w->sent_at = s->time_seconds;
    ++s->stats.data_transmissions; s->stats.data_bits_sent += (uint64_t)w->bytes * 8U;
    decide_fate(s, w, s->forward_route.reachable && s->forward_route.length > 1U ? s->forward_route.length - 1U : 1U);
    w->checksum = wire_checksum(s, w, false); f->checksum = w->checksum;
    trace_add(s, retransmission ? NR_TR_DATA_RETX : NR_TR_DATA_SEND, w->id, (int)seq, -1, (int)f->attempts, s->source, -1, (int)cause, 0);
    if (s->config.protocol == NR_ARQ_GO_BACK_N) { if (!s->gbn_timer_running) restart_gbn_timer(s); }
    else { ++f->timer_token; schedule(s, s->time_seconds + s->config.timeout_seconds, EV_TIMEOUT, (uint32_t)seq, f->timer_token); }
    forward_wire(s, index, s->source);
    return true;
}

static void sender_pump(NrArqSimulation *s) {
    if (s->state != NR_ARQ_RUNNING) return;
    const double rate = sending_rate(s);
    if (!(rate > 0.0)) return;
    const size_t window = nr_arq_effective_window(s);
    while (s->state == NR_ARQ_RUNNING && s->next_sequence < s->config.packet_count && s->next_sequence < s->sender_base + window) {
        if (s->time_seconds + 1e-12 < s->sender_next_allowed) {
            if (!s->wake_pending) { s->wake_pending = true; schedule(s, s->sender_next_allowed, EV_SENDER_WAKE, 0U, 0U); }
            return;
        }
        if (!send_data(s, s->next_sequence, NR_CAUSE_NONE)) return;
        ++s->next_sequence;
        s->sender_next_allowed = s->time_seconds + 1.0 / rate;
    }
}

static void go_back(NrArqSimulation *s, size_t from, NrRetxCause first_cause) {
    for (size_t i = from; i < s->next_sequence && s->state == NR_ARQ_RUNNING; ++i)
        if (!s->frames[i].acknowledged) (void)send_data(s, i, i == from ? first_cause : NR_CAUSE_GOBACK);
    if (s->state == NR_ARQ_RUNNING) restart_gbn_timer(s);
}

static void acknowledge_frame(NrArqSimulation *s, size_t seq) {
    NrArqFrame *f = &s->frames[seq];
    f->acknowledged = true; f->acked_at = s->time_seconds; ++f->timer_token;
    if (f->attempts == 1U) { /* Karn's rule: only unambiguous samples */
        const double rtt = (s->time_seconds - f->last_sent_at) * 1000.0;
        if (s->stats.rtt_samples == 0U || rtt < s->stats.rtt_min_ms) s->stats.rtt_min_ms = rtt;
        if (rtt > s->stats.rtt_max_ms) s->stats.rtt_max_ms = rtt;
        s->stats.rtt_sum_ms += rtt; ++s->stats.rtt_samples;
    }
    faults_on_acknowledged(s, seq);
    window_on_ack(s);
}

static void sender_handle(NrArqSimulation *s, const NrArqWire *w) {
    if (wire_checksum(s, w, true) != w->checksum) {
        ++s->stats.ack_corruptions;
        NrArqFault *fault = fault_for_frame(s, w);
        if (fault != NULL && !fault->recovered) { fault->detected = true; fault->detected_at = s->time_seconds; fault->response = NR_RESP_DISCARDED; ++s->fault_version; }
        else if (fault != NULL) { fault->detected = true; fault->detected_at = s->time_seconds; ++s->fault_version; }
        trace_add(s, NR_TR_ACK_CORRUPT_DETECTED, w->id, (int)w->sequence, (int)w->ack, (int)w->attempt, s->source, -1, 0, 0);
        return;
    }
    if (w->corrupted) {
        NrArqFault *fault = fault_for_frame(s, w);
        if (fault != NULL) { fault->response = NR_RESP_UNDETECTED; ++s->fault_version; }
    }
    const size_t n_packets = s->config.packet_count;
    if (w->kind == NR_ARQ_NAK_EVENT) {
        ++s->stats.negative_acks;
        trace_add(s, NR_TR_NAK_RECEIVED, w->id, (int)w->sequence, (int)w->ack, (int)w->attempt, s->source, -1, 0, 0);
        const size_t seq = w->sequence;
        if (seq >= n_packets || seq >= s->next_sequence || s->frames[seq].acknowledged) return; /* stale NAK */
        if (s->config.protocol == NR_ARQ_GO_BACK_N) go_back(s, seq, NR_CAUSE_NAK);
        else (void)send_data(s, seq, NR_CAUSE_NAK);
        return;
    }
    ++s->stats.acknowledgements;
    bool advanced = false;
    if (s->config.protocol == NR_ARQ_SELECTIVE_REPEAT) {
        const size_t seq = w->sequence;
        if (seq < n_packets && seq < s->next_sequence && !s->frames[seq].acknowledged) { acknowledge_frame(s, seq); advanced = true; }
        while (s->sender_base < n_packets && s->frames[s->sender_base].acknowledged) ++s->sender_base;
    } else {
        size_t ack = w->ack;
        if (ack > s->next_sequence) ack = s->next_sequence;
        if (ack > s->sender_base) {
            for (size_t i = s->sender_base; i < ack; ++i) if (!s->frames[i].acknowledged) acknowledge_frame(s, i);
            s->sender_base = ack; advanced = true;
            if (s->config.protocol == NR_ARQ_GO_BACK_N) {
                if (s->sender_base == s->next_sequence) { s->gbn_timer_running = false; ++s->gbn_timer_token; }
                else restart_gbn_timer(s);
            }
        }
    }
    if (!advanced) ++s->stats.dup_acks_received;
    trace_add(s, advanced ? NR_TR_ACK_RECEIVED : NR_TR_ACK_DUPLICATE, w->id, (int)w->sequence, (int)w->ack, (int)w->attempt, s->source, -1, 0, 0);
    if (s->sender_base >= n_packets) { complete_transfer(s); return; }
    sender_pump(s);
}

static void on_timeout(NrArqSimulation *s, size_t seq, uint32_t token) {
    if (seq >= s->config.packet_count) return;
    NrArqFrame *f = &s->frames[seq];
    if (f->timer_token != token || f->acknowledged || f->failed) return;
    ++s->stats.timeouts;
    trace_add(s, NR_TR_TIMEOUT, 0U, (int)seq, -1, (int)f->attempts, s->source, -1, 0, 0);
    window_on_timeout(s);
    (void)send_data(s, seq, NR_CAUSE_TIMEOUT);
}

static void on_gbn_timeout(NrArqSimulation *s, uint32_t token) {
    if (token != s->gbn_timer_token || !s->gbn_timer_running || s->sender_base >= s->next_sequence) return;
    ++s->stats.timeouts;
    trace_add(s, NR_TR_TIMEOUT, 0U, (int)s->sender_base, -1, (int)s->frames[s->sender_base].attempts, s->source, -1, 0, 0);
    window_on_timeout(s);
    go_back(s, s->sender_base, NR_CAUSE_TIMEOUT);
}

/* ---------------------------------------------------------------------------------------------
 * Receiver
 * ------------------------------------------------------------------------------------------- */
static void send_control(NrArqSimulation *s, NrArqEventType kind, unsigned seq, unsigned ack) {
    const unsigned occurrence = seq < s->config.packet_count ? ++s->frames[seq].ack_occurrences : 1U;
    const int index = wire_alloc(s);
    if (index < 0) { fail_transfer(s, NR_STOP_CAPACITY); return; }
    NrArqWire *w = &s->pool[index];
    w->kind = kind; w->sequence = seq; w->ack = ack; w->attempt = occurrence; w->bytes = NR_ARQ_ACK_BYTES;
    w->origin = s->destination; w->target = s->source; w->sent_at = s->time_seconds;
    if (kind == NR_ARQ_NAK_EVENT) ++s->stats.naks_sent; else ++s->stats.acks_sent;
    s->stats.control_bits_sent += (uint64_t)w->bytes * 8U;
    decide_fate(s, w, s->reverse_route.reachable && s->reverse_route.length > 1U ? s->reverse_route.length - 1U : 1U);
    w->checksum = wire_checksum(s, w, false);
    trace_add(s, kind == NR_ARQ_NAK_EVENT ? NR_TR_NAK_SEND : NR_TR_ACK_SEND, w->id, (int)seq, (int)ack, (int)occurrence, s->destination, -1, 0, 0);
    forward_wire(s, index, s->destination);
}

static void accept_unique(NrArqSimulation *s, size_t seq, unsigned attempt) {
    NrArqFrame *f = &s->frames[seq];
    f->received = true; f->received_at = s->time_seconds; f->accepted_attempt = attempt;
    ++s->stats.received_unique;
    faults_on_received(s, seq);
}

static void deliver(NrArqSimulation *s, size_t seq) {
    NrArqFrame *f = &s->frames[seq];
    f->delivered = true; f->delivered_at = s->time_seconds;
    ++s->stats.delivered;
    const double latency = (s->time_seconds - f->created_at) * 1000.0;
    if (s->stats.delivered == 1U || latency < s->stats.min_latency_ms) s->stats.min_latency_ms = latency;
    if (latency > s->stats.max_latency_ms) s->stats.max_latency_ms = latency;
    s->stats.total_latency_ms += latency; s->stats.latency_sq_sum += latency * latency;
    trace_add(s, NR_TR_DATA_DELIVERED, 0U, (int)seq, -1, (int)f->attempts, s->destination, -1, 0, 0);
}

static void receiver_handle(NrArqSimulation *s, const NrArqWire *w) {
    const size_t seq = w->sequence;
    if (seq >= s->config.packet_count) return;
    if (wire_checksum(s, w, true) != w->checksum) {
        ++s->stats.corruptions;
        const bool nak = s->config.use_nak && (s->config.protocol != NR_ARQ_GO_BACK_N || (seq == s->receiver_base && !s->gbn_nak_sent));
        NrArqFault *fault = fault_for_frame(s, w);
        if (fault != NULL) {
            fault->detected = true; fault->detected_at = s->time_seconds;
            fault->response = nak ? NR_RESP_NAK_SENT : NR_RESP_DISCARDED;
            if (s->frames[seq].received && !fault->recovered) { fault->recovered = true; fault->recovered_at = s->time_seconds; }
            ++s->fault_version;
        }
        trace_add(s, NR_TR_DATA_CORRUPT_DETECTED, w->id, (int)seq, -1, (int)w->attempt, s->destination, -1, nak ? 1 : 0, 0);
        if (nak) { if (s->config.protocol == NR_ARQ_GO_BACK_N) s->gbn_nak_sent = true; send_control(s, NR_ARQ_NAK_EVENT, (unsigned)seq, (unsigned)seq); }
        return;
    }
    if (w->corrupted) {
        ++s->stats.undetected_corruptions;
        NrArqFault *fault = fault_for_frame(s, w);
        if (fault != NULL) { fault->response = NR_RESP_UNDETECTED; ++s->fault_version; }
        trace_add(s, NR_TR_DATA_UNDETECTED_CORRUPTION, w->id, (int)seq, -1, (int)w->attempt, s->destination, -1, 0, 0);
    }
    if (s->config.protocol == NR_ARQ_SELECTIVE_REPEAT) {
        if (seq < s->receiver_base || s->receiver_buffer[seq]) {
            ++s->stats.duplicates; ++s->stats.dup_acks_sent;
            trace_add(s, NR_TR_DATA_DUPLICATE, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
            send_control(s, NR_ARQ_ACK_EVENT, (unsigned)seq, (unsigned)seq);
            return;
        }
        if (seq >= s->receiver_base + s->config.window_size) {
            ++s->stats.out_of_window;
            trace_add(s, NR_TR_DATA_OUT_OF_WINDOW, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
            return;
        }
        s->receiver_buffer[seq] = true;
        accept_unique(s, seq, w->attempt);
        if (seq > s->receiver_base) {
            ++s->stats.out_of_order;
            trace_add(s, NR_TR_DATA_BUFFERED, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
        } else trace_add(s, NR_TR_DATA_ACCEPTED, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
        while (s->receiver_base < s->config.packet_count && s->receiver_buffer[s->receiver_base]) deliver(s, s->receiver_base++);
        send_control(s, NR_ARQ_ACK_EVENT, (unsigned)seq, (unsigned)seq);
        return;
    }
    /* Stop-and-Wait and Go-Back-N receivers accept only the next expected frame; ACKs are cumulative. */
    if (seq == s->receiver_base) {
        s->receiver_buffer[seq] = true;
        accept_unique(s, seq, w->attempt);
        trace_add(s, NR_TR_DATA_ACCEPTED, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
        deliver(s, seq);
        ++s->receiver_base; s->gbn_nak_sent = false;
    } else if (seq < s->receiver_base) {
        ++s->stats.duplicates; ++s->stats.dup_acks_sent;
        trace_add(s, NR_TR_DATA_DUPLICATE, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
    } else {
        ++s->stats.out_of_order; ++s->stats.dup_acks_sent;
        trace_add(s, NR_TR_DATA_DISCARDED_OUT_OF_ORDER, w->id, (int)seq, (int)s->receiver_base, (int)w->attempt, s->destination, -1, 0, 0);
    }
    send_control(s, NR_ARQ_ACK_EVENT, (unsigned)seq, (unsigned)s->receiver_base);
}

/* ---------------------------------------------------------------------------------------------
 * Periodic monitor, outages and cross traffic
 * ------------------------------------------------------------------------------------------- */
static void on_monitor(NrArqSimulation *s) {
    route_queue_utilization(s);
    if (s->config.congestion_mode == NR_CC_RATE_AIMD) {
        const bool hold = s->queue_utilization >= 0.8 && s->stats.congestion_decreases > 0U &&
                          s->time_seconds - s->last_window_decrease < s->config.timeout_seconds;
        if (!hold) {
            const double before = s->congestion.rate; const unsigned d = s->congestion.decreases, i = s->congestion.increases;
            (void)nr_congestion_observe(&s->congestion, NR_ARQ_MONITOR_INTERVAL, s->queue_utilization, false);
            note_rate_change(s, before, d, i);
            if (s->congestion.rate > before) sender_pump(s);
        }
    }
    if (s->time_seconds + 1e-12 >= s->next_sample_at) sample_metrics(s);
    schedule(s, s->time_seconds + NR_ARQ_MONITOR_INTERVAL, EV_MONITOR, 0U, 0U);
}

static void on_outage(NrArqSimulation *s, size_t rule_index, bool start) {
    const NrFaultRule *r = &s->scenario.rules[rule_index];
    for (size_t j = 0; j < s->network->link_count; ++j) {
        if (s->network->links[j].id != r->link_id) continue;
        s->link_blocked[j] = start;
        trace_add(s, start ? NR_TR_LINK_DOWN : NR_TR_LINK_UP, 0U, -1, -1, 0, -1, r->link_id, 0, NR_SRC_SCENARIO);
        apply_topology_change(s);
        if (!start) sender_pump(s);
        return;
    }
}

static void on_cross_traffic(NrArqSimulation *s) {
    if (s->forward_route.reachable && s->forward_route.length >= 3U) {
        const int from = s->forward_route.nodes[1], to = s->forward_route.nodes[2];
        const int link = nr_find_link_between(s->network, from, to, s->link_blocked);
        if (link >= 0) {
            const int index = wire_alloc(s);
            if (index >= 0) {
                NrArqWire *w = &s->pool[index];
                w->kind = NR_ARQ_CROSS_EVENT; w->sequence = (unsigned)s->stats.cross_traffic_frames; w->bytes = s->config.cross_traffic_bytes;
                w->origin = from; w->target = to; w->next_node = to; w->link_index = link; w->sent_at = s->time_seconds;
                ++s->stats.cross_traffic_frames;
                forward_wire(s, index, from);
            }
        }
    }
    schedule(s, s->time_seconds + 1.0 / s->config.cross_traffic_pps, EV_CROSS, 0U, 0U);
}

static void dispatch(NrArqSimulation *s, const NrArqScheduled *ev) {
    switch (ev->type) {
    case EV_SENDER_WAKE: s->wake_pending = false; sender_pump(s); break;
    case EV_TX_DONE: on_tx_done(s, ev->a, ev->b); break;
    case EV_ARRIVE: on_arrive(s, (int)ev->a, ev->b); break;
    case EV_TIMEOUT: on_timeout(s, ev->a, ev->b); break;
    case EV_GBN_TIMEOUT: on_gbn_timeout(s, ev->b); break;
    case EV_MONITOR: on_monitor(s); break;
    case EV_OUTAGE_START: on_outage(s, ev->a, true); break;
    case EV_OUTAGE_END: on_outage(s, ev->a, false); break;
    case EV_CROSS: on_cross_traffic(s); break;
    default: break;
    }
}

static double time_limit(const NrArqSimulation *s) {
    return s->config.max_sim_time > 0.0 ? s->config.max_sim_time : NR_ARQ_DEFAULT_TIME_LIMIT;
}

static void process_one(NrArqSimulation *s) {
    const NrArqScheduled ev = heap_pop(s);
    if (ev.time > s->time_seconds) s->time_seconds = ev.time;
    dispatch(s, &ev);
}

/* ---------------------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------------------- */
void nr_arq_free(NrArqSimulation *s) {
    if (s == NULL) return;
    if (s->ports != NULL) for (size_t i = 0; i < s->port_count; ++i) free(s->ports[i].queue);
    free(s->ports); free(s->heap); free(s->pool); free(s->pool_free); free(s->trace); free(s->faults); free(s->scratch);
    s->ports = NULL; s->heap = NULL; s->pool = NULL; s->pool_free = NULL; s->trace = NULL; s->faults = NULL; s->scratch = NULL;
    s->port_count = s->heap_capacity = s->pool_capacity = s->pool_free_count = 0U;
    s->trace_count = s->trace_capacity = s->fault_count = s->fault_capacity = s->event_count = 0U;
}

static bool valid_probability(double p) { return isfinite(p) && p >= 0.0 && p <= 1.0; }

bool nr_arq_start(NrArqSimulation *s, const NrNetwork *network, const NrArqConfig *config, int source, int destination) {
    return nr_arq_start_scenario(s, network, config, NULL, source, destination);
}

bool nr_arq_start_scenario(NrArqSimulation *s, const NrNetwork *network, const NrArqConfig *config,
                           const NrFaultScenario *scenario, int source, int destination) {
    if (s == NULL || network == NULL || config == NULL || config->protocol < NR_ARQ_STOP_WAIT || config->protocol >= NR_ARQ_PROTOCOL_COUNT ||
        config->packet_count == 0U || config->packet_count > NR_ARQ_MAX_PACKETS || config->window_size == 0U || config->window_size > NR_ARQ_MAX_PACKETS ||
        config->payload_bytes == 0U || config->payload_bytes > NR_ARQ_MAX_PAYLOAD || !isfinite(config->packets_per_second) || config->packets_per_second < 0.0 ||
        !isfinite(config->timeout_seconds) || config->timeout_seconds <= 0.0 ||
        !valid_probability(config->loss_probability) || !valid_probability(config->corruption_probability) ||
        !valid_probability(config->ack_loss_probability) || !valid_probability(config->ack_corruption_probability) ||
        !isfinite(config->max_sim_time) || config->max_sim_time < 0.0 || config->congestion_mode < NR_CC_NONE || config->congestion_mode >= NR_CC_MODE_COUNT ||
        !isfinite(config->cross_traffic_pps) || config->cross_traffic_pps < 0.0 || config->cross_traffic_bytes > NR_ARQ_MAX_PAYLOAD ||
        config->corruption_bits > 64U || source == destination) return false;
    if (scenario != NULL && scenario->count > NR_ARQ_MAX_RULES) return false;
    const NrRoute forward = nr_find_route(network, source, destination), reverse = nr_find_route(network, destination, source);
    if (!forward.reachable || !reverse.reachable) return false;

    memset(s, 0, sizeof(*s));
    s->network = network; s->config = *config; s->source = source; s->destination = destination;
    if (scenario != NULL) s->scenario = *scenario;
    if (s->config.protocol == NR_ARQ_STOP_WAIT) s->config.window_size = 1U;
    if (s->config.congestion_control && s->config.congestion_mode == NR_CC_NONE) s->config.congestion_mode = NR_CC_RATE_AIMD;
    s->config.congestion_control = s->config.congestion_mode == NR_CC_RATE_AIMD;
    if (s->config.corruption_bits == 0U) s->config.corruption_bits = 1U;
    if (s->config.cross_traffic_bytes == 0U) s->config.cross_traffic_bytes = 1000U;

    s->heap_capacity = 256U; s->heap = malloc(s->heap_capacity * sizeof(*s->heap));
    s->pool_capacity = 64U; s->pool = malloc(s->pool_capacity * sizeof(*s->pool)); s->pool_free = malloc(s->pool_capacity * sizeof(*s->pool_free));
    s->port_count = network->link_count * 2U; s->ports = calloc(s->port_count == 0U ? 1U : s->port_count, sizeof(*s->ports));
    s->scratch_capacity = CHECKSUM_HEADER_BYTES + s->config.payload_bytes + 16U; s->scratch = malloc(s->scratch_capacity);
    if (s->heap == NULL || s->pool == NULL || s->pool_free == NULL || s->ports == NULL || s->scratch == NULL) { nr_arq_free(s); return false; }
    for (size_t i = s->pool_capacity; i > 0U; --i) { s->pool[i - 1U].state = NR_WIRE_FREE; s->pool_free[s->pool_free_count++] = (uint32_t)(i - 1U); }
    for (size_t i = 0; i < s->port_count; ++i) s->ports[i].transmitting = -1;
    for (size_t i = 0; i < NR_ARQ_MAX_PACKETS; ++i) s->frames[i].fault_head = -1;
    for (size_t i = 0; i < NR_MAX_LINKS; ++i) s->link_down_at[i] = -1.0;
    for (size_t j = 0; j < network->link_count; ++j) s->link_was_active[j] = network->links[j].active;
    s->topology_signature = topology_signature(network);
    s->forward_route = forward; s->reverse_route = reverse;
    s->recording = true;

    for (size_t i = 0; i < s->scenario.count; ++i) {
        const NrFaultRule *r = &s->scenario.rules[i];
        if (r->type == NR_RULE_QUEUE_LIMIT) {
            const int index = node_index(network, r->node_id);
            if (index >= 0) s->queue_override[index] = r->capacity == 0U ? 1U : r->capacity;
        }
    }
    nr_congestion_init(&s->congestion, s->config.packets_per_second);
    s->cwnd = s->config.congestion_mode == NR_CC_WINDOW_AIMD ? 1.0 : (double)s->config.window_size;
    s->ssthresh = fmax(1.0, (double)s->config.window_size / 2.0);
    s->stats.min_rate = s->config.packets_per_second;
    s->stats.min_window = (double)nr_arq_effective_window(s);
    s->history_interval = 0.1;
    s->state = NR_ARQ_RUNNING;
    trace_add(s, NR_TR_STARTED, 0U, -1, -1, 0, source, -1, 0, 0);

    schedule(s, 0.0, EV_SENDER_WAKE, 0U, 0U);
    schedule(s, NR_ARQ_MONITOR_INTERVAL, EV_MONITOR, 0U, 0U);
    if (s->config.cross_traffic_pps > 0.0) schedule(s, 0.0, EV_CROSS, 0U, 0U);
    for (size_t i = 0; i < s->scenario.count; ++i) {
        const NrFaultRule *r = &s->scenario.rules[i];
        if (r->type != NR_RULE_LINK_OUTAGE) continue;
        schedule(s, r->start, EV_OUTAGE_START, (uint32_t)i, 0U);
        schedule(s, r->end, EV_OUTAGE_END, (uint32_t)i, 0U);
    }
    s->wake_pending = true;
    sample_metrics(s);
    return true;
}

void nr_arq_step(NrArqSimulation *s, double dt) {
    if (s == NULL || s->state != NR_ARQ_RUNNING || !isfinite(dt) || dt <= 0.0) return;
    check_topology(s);
    const double target = s->time_seconds + dt, limit = time_limit(s);
    while (s->state == NR_ARQ_RUNNING && s->event_count > 0U && s->heap[0].time <= target && s->heap[0].time <= limit) process_one(s);
    if (s->state != NR_ARQ_RUNNING) return;
    if (target >= limit) { s->time_seconds = limit; fail_transfer(s, NR_STOP_TIME_LIMIT); return; }
    s->time_seconds = target;
}

bool nr_arq_step_event(NrArqSimulation *s) {
    if (s == NULL || (s->state != NR_ARQ_RUNNING && s->state != NR_ARQ_PAUSED) || s->event_count == 0U) return false;
    const NrArqState previous = s->state;
    s->state = NR_ARQ_RUNNING;
    check_topology(s);
    /* Skip silent bookkeeping events so a single step always shows a visible protocol action. */
    for (unsigned guard = 0U; guard < 100000U && s->state == NR_ARQ_RUNNING && s->event_count > 0U; ++guard) {
        if (s->heap[0].time > time_limit(s)) { s->time_seconds = time_limit(s); fail_transfer(s, NR_STOP_TIME_LIMIT); break; }
        const unsigned type = s->heap[0].type;
        const size_t trace_before = s->trace_count;
        process_one(s);
        if (type != EV_MONITOR && (type != EV_SENDER_WAKE || s->trace_count != trace_before)) break;
    }
    if (s->state == NR_ARQ_RUNNING && previous == NR_ARQ_PAUSED) s->state = NR_ARQ_PAUSED;
    return true;
}

void nr_arq_run(NrArqSimulation *s) {
    if (s == NULL) return;
    const double limit = time_limit(s);
    while (s->state == NR_ARQ_RUNNING) {
        if (s->event_count == 0U || s->heap[0].time > limit) { s->time_seconds = limit; fail_transfer(s, NR_STOP_TIME_LIMIT); break; }
        process_one(s);
    }
}

void nr_arq_pause(NrArqSimulation *s) { if (s != NULL && s->state == NR_ARQ_RUNNING) { s->state = NR_ARQ_PAUSED; trace_add(s, NR_TR_PAUSED, 0U, -1, -1, 0, -1, -1, 0, 0); } }
void nr_arq_resume(NrArqSimulation *s) { if (s != NULL && s->state == NR_ARQ_PAUSED) { s->state = NR_ARQ_RUNNING; trace_add(s, NR_TR_RESUMED, 0U, -1, -1, 0, -1, -1, 0, 0); } }
void nr_arq_stop(NrArqSimulation *s) {
    if (s != NULL && (s->state == NR_ARQ_RUNNING || s->state == NR_ARQ_PAUSED)) {
        s->state = NR_ARQ_STOPPED; s->stop_reason = NR_STOP_USER;
        trace_add(s, NR_TR_STOPPED, 0U, -1, -1, 0, -1, -1, 0, 0);
        sample_metrics(s);
    }
}

double nr_arq_delivery_ratio(const NrArqSimulation *s) { return s == NULL || s->stats.generated == 0U ? 0.0 : (double)s->stats.delivered / (double)s->stats.generated; }
double nr_arq_throughput_mbps(const NrArqSimulation *s) { return s == NULL || s->time_seconds <= 0.0 ? 0.0 : (double)s->stats.delivered * (double)s->config.payload_bytes * 8.0 / (s->time_seconds * 1000000.0); }
double nr_arq_efficiency(const NrArqSimulation *s) { return s == NULL || s->stats.data_transmissions == 0U ? 0.0 : (double)s->stats.delivered / (double)s->stats.data_transmissions; }

double nr_arq_packet_progress(const NrArqSimulation *s, unsigned sequence, bool *corrupted) {
    if (corrupted != NULL) *corrupted = false;
    if (s == NULL || sequence >= s->config.packet_count || s->pool == NULL) return 0.0;
    for (size_t i = 0; i < s->pool_capacity; ++i) {
        const NrArqWire *w = &s->pool[i];
        if (w->state != NR_WIRE_PROPAGATING || w->kind != NR_ARQ_DATA_EVENT || w->sequence != sequence) continue;
        if (corrupted != NULL) *corrupted = w->corrupted;
        const double duration = w->hop_end - w->hop_start;
        return duration <= 0.0 ? 1.0 : fmax(0.0, fmin(1.0, (s->time_seconds - w->hop_start) / duration));
    }
    return 0.0;
}

void nr_arq_compute_metrics(const NrArqSimulation *s, NrArqMetrics *m) {
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    if (s == NULL) return;
    const NrArqStatistics *st = &s->stats;
    m->completed = s->state == NR_ARQ_COMPLETE;
    m->stop_reason = s->stop_reason;
    m->packets_requested = s->config.packet_count;
    m->packets_delivered = st->delivered;
    m->packets_undelivered = m->packets_requested - m->packets_delivered;
    m->elapsed_seconds = s->time_seconds;
    const double payload_bits = (double)st->delivered * (double)s->config.payload_bytes * 8.0;
    const double wire_bits = (double)st->data_bits_sent + (double)st->control_bits_sent;
    m->has_delivery_ratio = m->packets_requested > 0U;
    m->delivery_ratio = m->has_delivery_ratio ? (double)st->delivered / (double)m->packets_requested : 0.0;
    m->has_goodput = m->elapsed_seconds > 0.0;
    if (m->has_goodput) {
        m->goodput_bps = payload_bits / m->elapsed_seconds;
        m->throughput_bps = wire_bits / m->elapsed_seconds;
        m->data_throughput_bps = (double)st->data_bits_sent / m->elapsed_seconds;
    }
    m->has_latency = st->delivered > 0U;
    if (m->has_latency) {
        const double n = (double)st->delivered;
        m->latency_avg_ms = st->total_latency_ms / n;
        m->latency_min_ms = st->min_latency_ms; m->latency_max_ms = st->max_latency_ms;
        const double variance = n > 1.0 ? (st->latency_sq_sum - n * m->latency_avg_ms * m->latency_avg_ms) / (n - 1.0) : 0.0;
        m->latency_stddev_ms = variance > 0.0 ? sqrt(variance) : 0.0;
    }
    m->first_transmissions = st->generated;
    m->data_transmissions = st->data_transmissions;
    m->has_retx_rate = st->generated > 0U;
    m->retransmission_rate = m->has_retx_rate ? (double)st->retransmissions / (double)st->generated : 0.0;
    m->has_efficiency = wire_bits > 0.0;
    m->efficiency = m->has_efficiency ? payload_bits / wire_bits : 0.0;
    m->frame_efficiency = st->data_transmissions == 0U ? 0.0 : (double)st->delivered / (double)st->data_transmissions;
    m->has_rtt = st->rtt_samples > 0U;
    m->rtt_avg_ms = m->has_rtt ? st->rtt_sum_ms / (double)st->rtt_samples : 0.0;
    m->retransmissions = st->retransmissions; m->retx_timeout = st->retx_timeout; m->retx_nak = st->retx_nak;
    m->retx_goback = st->retx_goback;
    /* Unnecessary (spurious) retransmissions: copies sent after the copy that the receiver accepted. */
    for (size_t i = 0; i < s->config.packet_count; ++i)
        if (s->frames[i].received && s->frames[i].attempts > s->frames[i].accepted_attempt) m->retx_unnecessary += s->frames[i].attempts - s->frames[i].accepted_attempt;
    m->acks_sent = st->acks_sent; m->naks_sent = st->naks_sent; m->dup_acks_sent = st->dup_acks_sent;
    m->timeouts = st->timeouts; m->duplicates_received = st->duplicates; m->out_of_order_received = st->out_of_order;
    m->data_bits = st->data_bits_sent; m->control_bits = st->control_bits_sent;
    for (size_t i = 0; i < s->fault_count; ++i) {
        const NrArqFault *f = &s->faults[i];
        NrFaultCounts *c = &m->faults[f->type];
        ++c->injected; ++m->faults_total;
        if (f->detected) ++c->detected;
        if (f->response == NR_RESP_NO_IMPACT) ++c->no_impact;
        if (f->recovered) { ++c->recovered; ++m->faults_recovered; } else { ++c->unrecovered; ++m->faults_unrecovered; }
    }
    size_t used = 0U; double total_avg = 0.0;
    for (size_t p = 0; p < s->port_count; ++p) {
        const NrArqPort *port = &s->ports[p];
        const NrLink *link = &s->network->links[p / 2U];
        const int owner = p % 2U == 0U ? link->from : link->to;
        if (owner == s->source || owner == s->destination) continue; /* only forwarding-node queues */
        if (port->forwarded == 0U && port->drops == 0U) continue;
        const double area = port->occupancy_area + (double)port->count * (s->time_seconds - port->last_change);
        const double average = s->time_seconds > 0.0 ? area / s->time_seconds : 0.0;
        total_avg += average; ++used;
        if (average > m->queue_max_avg_occupancy) m->queue_max_avg_occupancy = average;
        if (port->max_occupancy > m->queue_max_occupancy) m->queue_max_occupancy = port->max_occupancy;
    }
    m->queue_avg_occupancy = used == 0U ? 0.0 : total_avg / (double)used;
    m->queue_drops = st->queue_drops; m->data_queue_drops = st->data_queue_drops; m->ack_queue_drops = st->ack_queue_drops;
    m->congestion_decreases = st->congestion_decreases; m->congestion_increases = st->congestion_increases;
    m->min_rate = st->min_rate; m->min_window = st->min_window;
    m->cross_traffic_frames = st->cross_traffic_frames; m->cross_traffic_drops = st->cross_traffic_drops;
}

bool nr_arq_compare(const NrNetwork *network, const NrArqConfig *config, int source, int destination,
                    NrArqComparisonResult *go_back_n, NrArqComparisonResult *selective_repeat) {
    if (network == NULL || config == NULL || go_back_n == NULL || selective_repeat == NULL ||
        config->packets_per_second <= 0.0 || config->packet_count == 0U) return false;
    const NrArqProtocol protocols[2] = {NR_ARQ_GO_BACK_N, NR_ARQ_SELECTIVE_REPEAT};
    NrArqComparisonResult *results[2] = {go_back_n, selective_repeat};
    for (size_t run = 0; run < 2U; ++run) {
        NrArqConfig trial = *config;
        trial.protocol = protocols[run];
        NrArqSimulation *simulation = malloc(sizeof(*simulation));
        if (simulation == NULL) return false;
        if (!nr_arq_start(simulation, network, &trial, source, destination)) { free(simulation); return false; }
        simulation->recording = false;
        nr_arq_run(simulation);
        results[run]->completed = simulation->state == NR_ARQ_COMPLETE;
        results[run]->duration_seconds = simulation->time_seconds;
        results[run]->efficiency = nr_arq_efficiency(simulation);
        results[run]->statistics = simulation->stats;
        nr_arq_free(simulation);
        free(simulation);
    }
    return true;
}
