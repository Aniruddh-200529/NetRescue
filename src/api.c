#include "api.h"
#include "arq.h"
#include "experiment.h"
#include "json.h"
#include "netrescue.h"
#include "topology.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------------------------- */
static struct {
    bool initialized;
    NrNetwork network;
    int source, destination;
    NrArqConfig config;
    NrFaultScenario scenario;
    NrArqSimulation sim;
    bool sim_allocated;
    NrExperimentResult experiment;
    NrNetwork experiment_network;
    char experiment_topology[160];
    bool has_experiment;
    char *response;
} g;

static NrArqSimulation g_describer; /* shell used to describe trace records of experiment captures */

static const char *NODE_TYPES[] = {"host", "router", "switch", "server", "access_point"};
static const char *TOPOLOGIES[] = {"line", "bus", "star", "ring", "tree", "mesh", "hybrid", "random"};
static const char *PROTOCOLS[] = {"stop_and_wait", "go_back_n", "selective_repeat"};
static const char *COST_MODES[] = {"link", "delay", "composite"};
static const char *FRAME_KINDS[] = {"data", "ack", "nak", "cross"};

static int lookup(const char *const *names, int count, const char *name) {
    if (name == NULL) return -1;
    for (int i = 0; i < count; ++i) if (strcmp(names[i], name) == 0) return i;
    return -1;
}

static void ensure_init(void) {
    if (g.initialized) return;
    g.initialized = true;
    (void)nr_load_demo(&g.network);
    (void)nr_default_endpoints(&g.network, &g.source, &g.destination);
    nr_arq_default_config(&g.config);
}

static void free_sim(void) {
    if (g.sim_allocated) nr_arq_free(&g.sim);
    g.sim_allocated = false;
    memset(&g.sim, 0, sizeof(g.sim));
}

static bool sim_active(void) { return g.sim_allocated && (g.sim.state == NR_ARQ_RUNNING || g.sim.state == NR_ARQ_PAUSED); }

void nr_api_shutdown(void) {
    free_sim();
    if (g.has_experiment) nr_experiment_free(&g.experiment);
    free(g.response);
    memset(&g, 0, sizeof(g));
}

/* ---------------------------------------------------------------------------------------------
 * Parsing helpers
 * ------------------------------------------------------------------------------------------- */
typedef struct { char message[200]; } ApiError;

static bool set_error(ApiError *e, const char *message) { (void)snprintf(e->message, sizeof(e->message), "%s", message); return false; }

static bool has_number(const NrJson *object, const char *key) {
    const NrJson *v = nr_json_get(object, key);
    return v != NULL && v->type == NR_JSON_NUMBER;
}

static int json_int(const NrJson *object, const char *key, int fallback) {
    const NrJson *v = nr_json_get(object, key);
    return v != NULL && v->type == NR_JSON_NUMBER ? (int)lround(v->number) : fallback;
}

static bool parse_protocol(const NrJson *v, NrArqProtocol *out) {
    if (v == NULL) return false;
    if (v->type == NR_JSON_NUMBER && v->number >= 0 && v->number < NR_ARQ_PROTOCOL_COUNT) { *out = (NrArqProtocol)(int)v->number; return true; }
    const int index = lookup(PROTOCOLS, NR_ARQ_PROTOCOL_COUNT, nr_json_string(v, NULL));
    if (index < 0) return false;
    *out = (NrArqProtocol)index; return true;
}

static bool parse_congestion(const NrJson *v, NrCongestionMode *out) {
    if (v == NULL) return false;
    if (v->type == NR_JSON_NUMBER && v->number >= 0 && v->number < NR_CC_MODE_COUNT) { *out = (NrCongestionMode)(int)v->number; return true; }
    const char *name = nr_json_string(v, NULL);
    for (int i = 0; i < NR_CC_MODE_COUNT; ++i) if (name != NULL && strcmp(name, nr_congestion_mode_name((NrCongestionMode)i)) == 0) { *out = (NrCongestionMode)i; return true; }
    return false;
}

static bool number_in(const NrJson *o, const char *key, double lo, double hi, double *out, ApiError *e) {
    const NrJson *v = nr_json_get(o, key);
    if (v == NULL || v->type == NR_JSON_NULL) return true;
    if (v->type != NR_JSON_NUMBER || !isfinite(v->number) || v->number < lo || v->number > hi) {
        char message[160]; (void)snprintf(message, sizeof(message), "'%s' must be a number between %g and %g", key, lo, hi);
        return set_error(e, message);
    }
    *out = v->number; return true;
}

/* Applies the keys present in `o` on top of `c`; absent keys keep their current values. */
static bool parse_config(const NrJson *o, NrArqConfig *c, ApiError *e) {
    if (o == NULL || o->type == NR_JSON_NULL) return true;
    if (o->type != NR_JSON_OBJECT) return set_error(e, "config must be an object");
    double v;
    const NrJson *p = nr_json_get(o, "protocol");
    if (p != NULL && !parse_protocol(p, &c->protocol)) return set_error(e, "protocol must be stop_and_wait, go_back_n or selective_repeat");
    v = (double)c->packet_count; if (!number_in(o, "packet_count", 1, NR_ARQ_MAX_PACKETS, &v, e)) return false; c->packet_count = (size_t)v;
    v = c->window_size; if (!number_in(o, "window_size", 1, NR_ARQ_MAX_PACKETS, &v, e)) return false; c->window_size = (unsigned)v;
    v = c->payload_bytes; if (!number_in(o, "payload_bytes", 1, NR_ARQ_MAX_PAYLOAD, &v, e)) return false; c->payload_bytes = (unsigned)v;
    if (!number_in(o, "packets_per_second", 0, 100000, &c->packets_per_second, e)) return false;
    if (!number_in(o, "timeout_s", 0.0005, 60, &c->timeout_seconds, e)) return false;
    if (!number_in(o, "loss_probability", 0, 1, &c->loss_probability, e)) return false;
    if (!number_in(o, "corruption_probability", 0, 1, &c->corruption_probability, e)) return false;
    if (!number_in(o, "ack_loss_probability", 0, 1, &c->ack_loss_probability, e)) return false;
    if (!number_in(o, "ack_corruption_probability", 0, 1, &c->ack_corruption_probability, e)) return false;
    v = c->seed; if (!number_in(o, "seed", 0, 4294967295.0, &v, e)) return false; c->seed = (unsigned)v;
    v = c->max_retries; if (!number_in(o, "max_retries", 0, 1000, &v, e)) return false; c->max_retries = (unsigned)v;
    if (!number_in(o, "max_sim_time", 0, 100000, &c->max_sim_time, e)) return false;
    v = c->corruption_bits == 0U ? 1U : c->corruption_bits; if (!number_in(o, "corruption_bits", 1, 64, &v, e)) return false; c->corruption_bits = (unsigned)v;
    if (!number_in(o, "cross_traffic_pps", 0, 100000, &c->cross_traffic_pps, e)) return false;
    v = c->cross_traffic_bytes; if (!number_in(o, "cross_traffic_bytes", 1, NR_ARQ_MAX_PAYLOAD, &v, e)) return false; c->cross_traffic_bytes = (unsigned)v;
    if (nr_json_get(o, "use_nak") != NULL) c->use_nak = nr_json_bool(nr_json_get(o, "use_nak"), c->use_nak);
    const NrJson *cc = nr_json_get(o, "congestion_mode");
    if (cc != NULL) {
        if (!parse_congestion(cc, &c->congestion_mode)) return set_error(e, "congestion_mode must be none, rate_aimd or window_aimd");
        c->congestion_control = false;
    }
    return true;
}

static bool parse_scenario(const NrJson *a, NrFaultScenario *sc, ApiError *e) {
    memset(sc, 0, sizeof(*sc));
    if (a == NULL || a->type == NR_JSON_NULL) return true;
    if (a->type != NR_JSON_ARRAY) return set_error(e, "scenario must be an array of rules");
    for (size_t i = 0; i < a->count; ++i) {
        const NrJson *r = &a->items[i];
        NrFaultRule rule; memset(&rule, 0, sizeof(rule));
        const char *type = nr_json_string(nr_json_get(r, "type"), "");
        int t = -1;
        for (int k = 0; k < NR_RULE_TYPE_COUNT; ++k) if (strcmp(type, nr_fault_rule_name((NrFaultRuleType)k)) == 0) t = k;
        if (t < 0) return set_error(e, "unknown fault rule type (drop_data, corrupt_data, drop_ack, corrupt_ack, burst_loss, link_outage, queue_limit)");
        if (json_int(r, "sequence", 0) < 0 || json_int(r, "attempt", 1) < 0 || json_int(r, "count", 1) < 0 || json_int(r, "capacity", 1) < 0)
            return set_error(e, "rule sequence, attempt, count and capacity must be >= 0");
        rule.type = (NrFaultRuleType)t;
        rule.sequence = (unsigned)json_int(r, "sequence", 0); rule.attempt = (unsigned)json_int(r, "attempt", 1);
        rule.count = (unsigned)json_int(r, "count", 1); rule.capacity = (unsigned)json_int(r, "capacity", 1);
        rule.link_id = json_int(r, "link_id", -1); rule.node_id = json_int(r, "node_id", -1);
        rule.start = nr_json_number(nr_json_get(r, "start"), 0.0); rule.end = nr_json_number(nr_json_get(r, "end"), 0.0);
        if (rule.type == NR_RULE_LINK_OUTAGE && rule.link_id < 0) return set_error(e, "link_outage needs link_id");
        if (rule.type == NR_RULE_QUEUE_LIMIT && rule.node_id < 0) return set_error(e, "queue_limit needs node_id");
        if (!nr_scenario_add(sc, &rule)) return set_error(e, "invalid rule (at most 32 rules; an outage needs 0 <= start < end)");
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Writers
 * ------------------------------------------------------------------------------------------- */
static void write_config(NrJsonWriter *w, const NrArqConfig *c) {
    jw_begin_object(w);
    jw_kv_string(w, "protocol", PROTOCOLS[c->protocol]);
    jw_kv_uint(w, "packet_count", c->packet_count); jw_kv_uint(w, "window_size", c->window_size);
    jw_kv_uint(w, "payload_bytes", c->payload_bytes); jw_kv_number(w, "packets_per_second", c->packets_per_second);
    jw_kv_number(w, "timeout_s", c->timeout_seconds);
    jw_kv_number(w, "loss_probability", c->loss_probability); jw_kv_number(w, "corruption_probability", c->corruption_probability);
    jw_kv_number(w, "ack_loss_probability", c->ack_loss_probability); jw_kv_number(w, "ack_corruption_probability", c->ack_corruption_probability);
    jw_kv_uint(w, "seed", c->seed); jw_kv_uint(w, "max_retries", c->max_retries); jw_kv_number(w, "max_sim_time", c->max_sim_time);
    jw_kv_string(w, "congestion_mode", nr_congestion_mode_name(c->congestion_control && c->congestion_mode == NR_CC_NONE ? NR_CC_RATE_AIMD : c->congestion_mode));
    jw_kv_bool(w, "use_nak", c->use_nak); jw_kv_uint(w, "corruption_bits", c->corruption_bits == 0U ? 1U : c->corruption_bits);
    jw_kv_number(w, "cross_traffic_pps", c->cross_traffic_pps); jw_kv_uint(w, "cross_traffic_bytes", c->cross_traffic_bytes);
    jw_end_object(w);
}

static void write_scenario(NrJsonWriter *w, const NrFaultScenario *sc) {
    jw_begin_array(w);
    for (size_t i = 0; i < sc->count; ++i) {
        const NrFaultRule *r = &sc->rules[i];
        jw_begin_object(w);
        jw_kv_string(w, "type", nr_fault_rule_name(r->type));
        switch (r->type) {
        case NR_RULE_LINK_OUTAGE: jw_kv_int(w, "link_id", r->link_id); jw_kv_number(w, "start", r->start); jw_kv_number(w, "end", r->end); break;
        case NR_RULE_QUEUE_LIMIT: jw_kv_int(w, "node_id", r->node_id); jw_kv_uint(w, "capacity", r->capacity); break;
        case NR_RULE_BURST_LOSS: jw_kv_uint(w, "sequence", r->sequence); jw_kv_uint(w, "count", r->count); jw_kv_uint(w, "attempt", r->attempt); break;
        default: jw_kv_uint(w, "sequence", r->sequence); jw_kv_uint(w, "attempt", r->attempt); break;
        }
        jw_end_object(w);
    }
    jw_end_array(w);
}

static void write_route(NrJsonWriter *w, const NrRoute *r) {
    jw_begin_array(w);
    for (size_t i = 0; r->reachable && i < r->length; ++i) jw_int(w, r->nodes[i]);
    jw_end_array(w);
}

static void write_topology(NrJsonWriter *w, const NrNetwork *n, int source, int destination) {
    jw_key(w, "nodes"); jw_begin_array(w);
    for (size_t i = 0; i < n->node_count; ++i) {
        const NrNode *v = &n->nodes[i];
        jw_begin_object(w);
        jw_kv_int(w, "id", v->id); jw_kv_string(w, "name", v->name); jw_kv_string(w, "type", NODE_TYPES[v->type]);
        jw_kv_number(w, "x", v->x); jw_kv_number(w, "y", v->y); jw_kv_bool(w, "active", v->active);
        jw_kv_string(w, "ip", v->ip); jw_kv_uint(w, "queue_capacity", v->queue_capacity);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_key(w, "links"); jw_begin_array(w);
    for (size_t i = 0; i < n->link_count; ++i) {
        const NrLink *l = &n->links[i];
        jw_begin_object(w);
        jw_kv_int(w, "id", l->id); jw_kv_int(w, "from", l->from); jw_kv_int(w, "to", l->to);
        jw_kv_bool(w, "bidirectional", l->bidirectional); jw_kv_bool(w, "active", l->active);
        jw_kv_number(w, "cost", l->cost); jw_kv_number(w, "delay_ms", l->delay_ms); jw_kv_number(w, "bandwidth_mbps", l->bandwidth_mbps);
        jw_kv_number(w, "loss_probability", l->loss_probability); jw_kv_number(w, "jitter_ms", l->jitter_ms);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_kv_int(w, "source", source); jw_kv_int(w, "destination", destination);
    const NrRoute fwd = nr_find_route(n, source, destination), rev = nr_find_route(n, destination, source);
    jw_key(w, "route"); write_route(w, &fwd);
    jw_key(w, "reverse_route"); write_route(w, &rev);
    jw_kv_opt(w, "route_cost", fwd.reachable, fwd.cost);
}

static void write_network(NrJsonWriter *w) {
    jw_begin_object(w);
    write_topology(w, &g.network, g.source, g.destination);
    jw_kv_string(w, "cost_mode", COST_MODES[g.network.cost_mode]);
    jw_kv_bool(w, "locked", sim_active());
    const NrRoute fwd = nr_find_route(&g.network, g.source, g.destination), rev = nr_find_route(&g.network, g.destination, g.source);
    const double rtt = fwd.reachable && rev.reachable ? nr_route_base_rtt_ms(&g.network, &fwd, &rev, g.config.payload_bytes) : NAN;
    jw_kv_opt(w, "base_rtt_ms", isfinite(rtt), rtt);
    NrIssue issues[NR_MAX_ISSUES];
    const size_t count = nr_validate_network(&g.network, g.source, g.destination, issues);
    jw_key(w, "issues"); jw_begin_array(w);
    for (size_t i = 0; i < count; ++i) {
        static const char *severity[] = {"info", "warning", "error"};
        jw_begin_object(w);
        jw_kv_string(w, "severity", severity[issues[i].severity]); jw_kv_string(w, "message", issues[i].message);
        jw_kv_int(w, "node_id", issues[i].node_id); jw_kv_int(w, "link_id", issues[i].link_id);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_end_object(w);
}

static void write_fault_counts(NrJsonWriter *w, const NrFaultCounts *c) {
    jw_begin_object(w);
    jw_kv_uint(w, "injected", c->injected); jw_kv_uint(w, "detected", c->detected); jw_kv_uint(w, "recovered", c->recovered);
    jw_kv_uint(w, "unrecovered", c->unrecovered); jw_kv_uint(w, "no_impact", c->no_impact);
    jw_end_object(w);
}

static void write_metrics(NrJsonWriter *w, const NrArqMetrics *m) {
    jw_begin_object(w);
    jw_kv_bool(w, "completed", m->completed); jw_kv_string(w, "stop_reason", nr_arq_stop_reason_name(m->stop_reason));
    jw_kv_uint(w, "packets_requested", m->packets_requested); jw_kv_uint(w, "packets_delivered", m->packets_delivered);
    jw_kv_uint(w, "packets_undelivered", m->packets_undelivered); jw_kv_number(w, "elapsed_s", m->elapsed_seconds);
    jw_kv_opt(w, "delivery_ratio", m->has_delivery_ratio, m->delivery_ratio);
    jw_kv_opt(w, "goodput_bps", m->has_goodput, m->goodput_bps);
    jw_kv_opt(w, "throughput_bps", m->has_goodput, m->throughput_bps);
    jw_kv_opt(w, "data_throughput_bps", m->has_goodput, m->data_throughput_bps);
    jw_kv_opt(w, "latency_avg_ms", m->has_latency, m->latency_avg_ms);
    jw_kv_opt(w, "latency_min_ms", m->has_latency, m->latency_min_ms);
    jw_kv_opt(w, "latency_max_ms", m->has_latency, m->latency_max_ms);
    jw_kv_opt(w, "latency_stddev_ms", m->has_latency, m->latency_stddev_ms);
    jw_kv_opt(w, "retransmission_rate", m->has_retx_rate, m->retransmission_rate);
    jw_kv_opt(w, "efficiency", m->has_efficiency, m->efficiency);
    jw_kv_opt(w, "frame_efficiency", m->data_transmissions > 0U, m->frame_efficiency);
    jw_kv_opt(w, "rtt_avg_ms", m->has_rtt, m->rtt_avg_ms);
    jw_kv_uint(w, "retransmissions", m->retransmissions); jw_kv_uint(w, "retx_timeout", m->retx_timeout);
    jw_kv_uint(w, "retx_nak", m->retx_nak); jw_kv_uint(w, "retx_goback", m->retx_goback); jw_kv_uint(w, "retx_unnecessary", m->retx_unnecessary);
    jw_kv_uint(w, "data_transmissions", m->data_transmissions); jw_kv_uint(w, "first_transmissions", m->first_transmissions);
    jw_kv_uint(w, "acks_sent", m->acks_sent); jw_kv_uint(w, "naks_sent", m->naks_sent); jw_kv_uint(w, "dup_acks_sent", m->dup_acks_sent);
    jw_kv_uint(w, "timeouts", m->timeouts); jw_kv_uint(w, "duplicates_received", m->duplicates_received);
    jw_kv_uint(w, "out_of_order_received", m->out_of_order_received);
    jw_kv_uint(w, "data_bits", m->data_bits); jw_kv_uint(w, "control_bits", m->control_bits);
    jw_key(w, "faults"); jw_begin_object(w);
    for (int t = 0; t < NR_FAULT_TYPE_COUNT; ++t) { jw_key(w, nr_fault_type_name((NrFaultType)t)); write_fault_counts(w, &m->faults[t]); }
    jw_end_object(w);
    jw_kv_uint(w, "faults_total", m->faults_total); jw_kv_uint(w, "faults_recovered", m->faults_recovered);
    jw_kv_uint(w, "faults_unrecovered", m->faults_unrecovered);
    jw_kv_number(w, "queue_avg_occupancy", m->queue_avg_occupancy); jw_kv_number(w, "queue_max_avg_occupancy", m->queue_max_avg_occupancy);
    jw_kv_uint(w, "queue_max_occupancy", m->queue_max_occupancy); jw_kv_uint(w, "queue_drops", m->queue_drops);
    jw_kv_uint(w, "data_queue_drops", m->data_queue_drops); jw_kv_uint(w, "ack_queue_drops", m->ack_queue_drops);
    jw_kv_uint(w, "congestion_decreases", m->congestion_decreases); jw_kv_uint(w, "congestion_increases", m->congestion_increases);
    jw_kv_number(w, "min_rate", m->min_rate); jw_kv_number(w, "min_window", m->min_window);
    jw_kv_uint(w, "cross_traffic_frames", m->cross_traffic_frames); jw_kv_uint(w, "cross_traffic_drops", m->cross_traffic_drops);
    jw_end_object(w);
}

static void write_trace_record(NrJsonWriter *w, const NrArqSimulation *s, const NrArqTrace *r, size_t index) {
    char text[NR_EVENT_LEN];
    nr_arq_trace_describe(s, r, text, sizeof(text));
    jw_begin_object(w);
    jw_kv_uint(w, "i", index); jw_kv_number(w, "t", r->time); jw_kv_string(w, "type", nr_arq_trace_name(r->type));
    jw_kv_int(w, "seq", r->sequence); jw_kv_int(w, "ack", r->ack); jw_kv_int(w, "attempt", r->attempt);
    jw_kv_uint(w, "frame", r->frame_id); jw_kv_int(w, "node", r->node_id); jw_kv_int(w, "link", r->link_id);
    jw_kv_int(w, "detail", r->detail); jw_kv_string(w, "text", text);
    if (r->type == NR_TR_DATA_RETX) jw_kv_string(w, "cause", nr_retx_cause_name((NrRetxCause)r->detail));
    if (r->type == NR_TR_DATA_LOST || r->type == NR_TR_ACK_LOST || r->type == NR_TR_CORRUPTION_INJECTED) {
        jw_kv_string(w, "fault_source", nr_fault_source_name((NrFaultSource)r->source));
        if (r->type != NR_TR_CORRUPTION_INJECTED) jw_kv_string(w, "fault_type", nr_fault_type_name(r->detail < 0 ? NR_FAULT_ACK_LOSS : (NrFaultType)r->detail));
    }
    jw_end_object(w);
}

static void write_fault(NrJsonWriter *w, const NrArqFault *f, size_t index) {
    jw_begin_object(w);
    jw_kv_uint(w, "i", index); jw_kv_number(w, "t", f->time); jw_kv_string(w, "type", nr_fault_type_name(f->type));
    jw_kv_string(w, "source", nr_fault_source_name(f->source)); jw_kv_string(w, "frame_kind", FRAME_KINDS[f->frame_kind]);
    jw_kv_uint(w, "frame", f->frame_id); jw_kv_uint(w, "seq", f->sequence); jw_kv_uint(w, "attempt", f->attempt);
    jw_kv_int(w, "node", f->node_id); jw_kv_int(w, "link", f->link_id);
    jw_kv_bool(w, "detected", f->detected); jw_kv_opt(w, "detected_at", f->detected, f->detected_at);
    jw_kv_string(w, "response", nr_fault_response_name(f->response));
    jw_kv_bool(w, "recovered", f->recovered); jw_kv_opt(w, "recovered_at", f->recovered, f->recovered_at);
    jw_end_object(w);
}

static void write_history(NrJsonWriter *w, const NrArqSimulation *s) {
    jw_begin_array(w);
    for (size_t i = 0; i < s->history_count; ++i) {
        const NrArqSample *h = &s->history[i];
        jw_begin_object(w);
        jw_kv_number(w, "t", h->time_seconds); jw_kv_number(w, "delivery_ratio", h->delivery_ratio);
        jw_kv_number(w, "goodput_mbps", h->throughput_mbps); jw_kv_number(w, "latency_ms", h->average_latency_ms);
        jw_kv_number(w, "queue_utilization", h->queue_utilization); jw_kv_number(w, "loss_rate", h->packet_loss_rate);
        jw_kv_number(w, "window", h->window); jw_kv_number(w, "rate", h->rate);
        jw_kv_uint(w, "retransmissions", h->retransmissions); jw_kv_uint(w, "delivered", h->delivered); jw_kv_uint(w, "outstanding", h->in_flight);
        jw_end_object(w);
    }
    jw_end_array(w);
}

/* Live state of the session simulation. Trace records and faults are paged by the caller's cursors. */
static void write_sim_state(NrJsonWriter *w, const NrJson *req) {
    jw_begin_object(w);
    if (!g.sim_allocated) { jw_kv_string(w, "state", "Idle"); jw_kv_bool(w, "active", false); jw_end_object(w); return; }
    NrArqSimulation *s = &g.sim;
    jw_kv_string(w, "state", nr_arq_state_name(s->state)); jw_kv_bool(w, "active", sim_active());
    jw_kv_string(w, "stop_reason", nr_arq_stop_reason_name(s->stop_reason));
    jw_kv_number(w, "time", s->time_seconds); jw_kv_string(w, "protocol", PROTOCOLS[s->config.protocol]);
    jw_kv_uint(w, "pending_events", s->event_count);
    jw_key(w, "config"); write_config(w, &s->config);
    jw_key(w, "sender"); jw_begin_object(w);
    jw_kv_uint(w, "base", s->sender_base); jw_kv_uint(w, "next", s->next_sequence);
    jw_kv_uint(w, "window", s->config.window_size); jw_kv_uint(w, "effective_window", nr_arq_effective_window(s));
    jw_kv_number(w, "cwnd", s->cwnd); jw_kv_number(w, "rate", s->config.congestion_mode == NR_CC_RATE_AIMD ? s->congestion.rate : s->config.packets_per_second);
    jw_kv_bool(w, "gbn_timer_running", s->gbn_timer_running);
    jw_end_object(w);
    jw_key(w, "receiver"); jw_begin_object(w); jw_kv_uint(w, "base", s->receiver_base); jw_end_object(w);
    /* Per-packet status strings: sender n=not sent o=outstanding a=acknowledged f=failed; receiver -=missing b=buffered d=delivered. */
    char *map = malloc(s->config.packet_count + 1U);
    if (map != NULL) {
        for (size_t i = 0; i < s->config.packet_count; ++i) {
            const NrArqFrame *f = &s->frames[i];
            map[i] = f->failed ? 'f' : f->acknowledged ? 'a' : f->attempts > 0U ? 'o' : 'n';
        }
        map[s->config.packet_count] = '\0'; jw_kv_string(w, "sender_map", map);
        for (size_t i = 0; i < s->config.packet_count; ++i) map[i] = s->frames[i].delivered ? 'd' : s->frames[i].received ? 'b' : '-';
        jw_kv_string(w, "receiver_map", map);
        for (size_t i = 0; i < s->config.packet_count; ++i) { const unsigned a = s->frames[i].attempts; map[i] = (char)('0' + (a > 9U ? 9U : a)); }
        jw_kv_string(w, "attempts_map", map);
        free(map);
    }
    jw_key(w, "route"); write_route(w, &s->forward_route);
    jw_key(w, "reverse_route"); write_route(w, &s->reverse_route);
    jw_key(w, "blocked_links"); jw_begin_array(w);
    for (size_t j = 0; j < s->network->link_count; ++j) if (s->link_blocked[j]) jw_int(w, s->network->links[j].id);
    jw_end_array(w);
    jw_key(w, "in_flight"); jw_begin_array(w);
    size_t shown = 0U;
    for (size_t i = 0; i < s->pool_capacity && shown < 600U; ++i) {
        const NrArqWire *wire = &s->pool[i];
        if (wire->state == NR_WIRE_FREE) continue;
        ++shown;
        jw_begin_object(w);
        jw_kv_uint(w, "id", wire->id); jw_kv_string(w, "kind", FRAME_KINDS[wire->kind]);
        jw_kv_uint(w, "seq", wire->sequence); jw_kv_uint(w, "ack", wire->ack); jw_kv_uint(w, "attempt", wire->attempt);
        jw_kv_string(w, "phase", wire->state == NR_WIRE_QUEUED ? "queued" : wire->state == NR_WIRE_TRANSMITTING ? "transmitting" : "propagating");
        jw_kv_int(w, "from", wire->at_node); jw_kv_int(w, "to", wire->next_node);
        jw_kv_int(w, "link", wire->link_index >= 0 ? s->network->links[wire->link_index].id : -1);
        const double span = wire->hop_end - wire->hop_start;
        const double progress = wire->state == NR_WIRE_QUEUED ? 0.0 : span <= 0.0 ? 1.0 : fmax(0.0, fmin(1.0, (s->time_seconds - wire->hop_start) / span));
        jw_kv_number(w, "progress", progress); jw_kv_bool(w, "corrupted", wire->corrupted);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_key(w, "ports"); jw_begin_array(w);
    for (size_t p = 0; p < s->port_count; ++p) {
        const NrArqPort *port = &s->ports[p];
        if (port->count == 0U && port->transmitting < 0 && port->drops == 0U && port->forwarded == 0U) continue;
        const NrLink *link = &s->network->links[p / 2U];
        jw_begin_object(w);
        jw_kv_int(w, "link", link->id); jw_kv_int(w, "node", p % 2U == 0U ? link->from : link->to);
        jw_kv_int(w, "toward", p % 2U == 0U ? link->to : link->from);
        jw_kv_uint(w, "queued", port->count); jw_kv_uint(w, "capacity", nr_arq_port_capacity(s, p));
        jw_kv_bool(w, "transmitting", port->transmitting >= 0); jw_kv_uint(w, "max", port->max_occupancy);
        jw_kv_uint(w, "drops", port->drops); jw_kv_uint(w, "forwarded", port->forwarded);
        jw_end_object(w);
    }
    jw_end_array(w);
    NrArqMetrics m; nr_arq_compute_metrics(s, &m);
    jw_key(w, "metrics"); write_metrics(w, &m);
    /* Trace page. */
    size_t from = (size_t)fmax(0.0, nr_json_number(nr_json_get(req, "trace_from"), 0.0));
    const size_t limit = (size_t)fmax(1.0, fmin(5000.0, nr_json_number(nr_json_get(req, "trace_limit"), 1500.0)));
    if (from > s->trace_count) from = s->trace_count;
    jw_kv_uint(w, "trace_total", s->trace_count); jw_kv_uint(w, "trace_from", from);
    jw_kv_uint(w, "trace_dropped", s->trace_dropped);
    jw_key(w, "trace"); jw_begin_array(w);
    for (size_t i = from; i < s->trace_count && i < from + limit; ++i) write_trace_record(w, s, &s->trace[i], i);
    jw_end_array(w);
    /* Faults and history only when they changed since the caller's versions. */
    const double known_faults = nr_json_number(nr_json_get(req, "fault_version"), -1.0);
    jw_kv_uint(w, "fault_version", s->fault_version);
    if ((double)s->fault_version != known_faults) {
        jw_key(w, "faults"); jw_begin_array(w);
        const size_t first = s->fault_count > 2000U ? s->fault_count - 2000U : 0U;
        for (size_t i = first; i < s->fault_count; ++i) write_fault(w, &s->faults[i], i);
        jw_end_array(w);
    }
    const double known_history = nr_json_number(nr_json_get(req, "history_version"), -1.0);
    jw_kv_uint(w, "history_version", s->history_version);
    if ((double)s->history_version != known_history) { jw_key(w, "history"); write_history(w, s); }
    jw_end_object(w);
}

/* ---------------------------------------------------------------------------------------------
 * Experiments
 * ------------------------------------------------------------------------------------------- */
static bool parse_experiment(const NrJson *req, NrExperimentSpec *spec, NrNetwork *network, char *topology_label, size_t label_size, ApiError *e) {
    /* Experiments are self-contained: unspecified settings come from the engine defaults, never from the
     * live-lab session, so the same request always produces the same result. */
    nr_experiment_default_spec(spec);
    const NrJson *topology = nr_json_get(req, "topology");
    const char *kind = nr_json_string(nr_json_get(topology, "kind"), "current");
    bool preset_endpoints = false;
    if (strcmp(kind, "preset") == 0) {
        const int index = nr_preset_find(nr_json_string(nr_json_get(topology, "id"), ""));
        if (index < 0 || !nr_preset_build((size_t)index, network, spec)) return set_error(e, "unknown preset id");
        (void)snprintf(topology_label, label_size, "Preset topology: %s", nr_preset_info((size_t)index)->title);
        preset_endpoints = true;
    } else if (strcmp(kind, "generate") == 0) {
        const int type = lookup(TOPOLOGIES, NR_TOPO_COUNT, nr_json_string(nr_json_get(topology, "type"), "ring"));
        const int nodes = json_int(topology, "nodes", 8);
        const unsigned seed = (unsigned)json_int(topology, "seed", 1);
        if (type < 0 || !nr_generate_topology(network, (NrTopologyType)type, (size_t)nodes, seed))
            return set_error(e, "could not generate topology (check type and node count 2-64; mesh allows at most 23 nodes)");
        (void)nr_default_endpoints(network, &spec->source, &spec->destination);
        (void)snprintf(topology_label, label_size, "Generated %s topology, %d nodes, seed %u", nr_topology_name((NrTopologyType)type), nodes, seed);
    } else if (strcmp(kind, "current") == 0) {
        *network = g.network; spec->source = g.source; spec->destination = g.destination;
        (void)snprintf(topology_label, label_size, "Current lab topology (%zu nodes, %zu links)", network->node_count, network->link_count);
    } else return set_error(e, "topology.kind must be current, generate or preset");
    (void)preset_endpoints;
    if (has_number(req, "source")) spec->source = json_int(req, "source", spec->source);
    if (has_number(req, "destination")) spec->destination = json_int(req, "destination", spec->destination);
    if (!parse_config(nr_json_get(req, "config"), &spec->config, e)) return false;
    if (nr_json_get(req, "scenario") != NULL && !parse_scenario(nr_json_get(req, "scenario"), &spec->scenario, e)) return false;
    const NrJson *protocols = nr_json_get(req, "protocols");
    if (protocols != NULL) {
        spec->protocol_mask = 0U;
        if (protocols->type != NR_JSON_ARRAY) return set_error(e, "protocols must be an array");
        for (size_t i = 0; i < protocols->count; ++i) {
            NrArqProtocol p;
            if (!parse_protocol(&protocols->items[i], &p)) return set_error(e, "unknown protocol in protocols");
            spec->protocol_mask |= 1U << p;
        }
    }
    double v = spec->trials; if (!number_in(req, "trials", 1, NR_EXP_MAX_TRIALS, &v, e)) return false; spec->trials = (unsigned)v;
    v = spec->seed_base; if (!number_in(req, "seed_base", 0, 4294967295.0, &v, e)) return false; spec->seed_base = (unsigned)v;
    v = spec->seed_stride; if (!number_in(req, "seed_stride", 0, 4294967295.0, &v, e)) return false; spec->seed_stride = (unsigned)v;
    if (nr_json_get(req, "capture_trace") != NULL) spec->capture_trace = nr_json_bool(nr_json_get(req, "capture_trace"), false);
    const NrJson *sweep = nr_json_get(req, "sweep");
    if (sweep != NULL && sweep->type == NR_JSON_OBJECT) {
        const char *name = nr_json_string(nr_json_get(sweep, "parameter"), "none");
        if (!nr_sweep_from_name(name, &spec->sweep)) return set_error(e, "unknown sweep parameter");
        const NrJson *values = nr_json_get(sweep, "values");
        spec->sweep_count = 0U;
        if (spec->sweep != NR_SWEEP_NONE) {
            if (values == NULL || values->type != NR_JSON_ARRAY || values->count == 0U || values->count > NR_EXP_MAX_SWEEP)
                return set_error(e, "sweep.values must be an array of 1-12 numbers");
            for (size_t i = 0; i < values->count; ++i) {
                if (values->items[i].type != NR_JSON_NUMBER) return set_error(e, "sweep.values must be numbers");
                spec->sweep_values[spec->sweep_count++] = values->items[i].number;
            }
        }
    }
    const NrJson *overrides = nr_json_get(req, "overrides");
    if (overrides != NULL) {
        if (!number_in(overrides, "link_delay_ms", 0, 60000, &spec->link_delay_ms, e)) return false;
        if (!number_in(overrides, "link_bandwidth_mbps", 0.001, 100000, &spec->link_bandwidth_mbps, e)) return false;
        if (!number_in(overrides, "link_jitter_ms", 0, 60000, &spec->link_jitter_ms, e)) return false;
        double q = spec->queue_capacity; if (!number_in(overrides, "queue_capacity", 0, NR_ARQ_MAX_QUEUE, &q, e)) return false; spec->queue_capacity = (int)q;
    }
    return true;
}

static void write_spec(NrJsonWriter *w, const NrExperimentSpec *spec) {
    jw_begin_object(w);
    jw_key(w, "config"); write_config(w, &spec->config);
    jw_key(w, "scenario"); write_scenario(w, &spec->scenario);
    jw_key(w, "protocols"); jw_begin_array(w);
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) if (spec->protocol_mask & (1U << p)) jw_string(w, PROTOCOLS[p]);
    jw_end_array(w);
    jw_kv_uint(w, "trials", spec->trials); jw_kv_uint(w, "seed_base", spec->seed_base); jw_kv_uint(w, "seed_stride", spec->seed_stride);
    jw_kv_string(w, "seed_schedule", "seed(trial i) = seed_base + i * seed_stride, identical for every protocol and sweep value");
    jw_key(w, "sweep"); jw_begin_object(w);
    jw_kv_string(w, "parameter", nr_sweep_name(spec->sweep));
    jw_key(w, "values"); jw_begin_array(w); for (size_t i = 0; i < spec->sweep_count; ++i) jw_number(w, spec->sweep_values[i]); jw_end_array(w);
    jw_end_object(w);
    jw_key(w, "overrides"); jw_begin_object(w);
    jw_kv_opt(w, "link_delay_ms", spec->link_delay_ms >= 0.0, spec->link_delay_ms);
    jw_kv_opt(w, "link_bandwidth_mbps", spec->link_bandwidth_mbps >= 0.0, spec->link_bandwidth_mbps);
    jw_kv_opt(w, "link_jitter_ms", spec->link_jitter_ms >= 0.0, spec->link_jitter_ms);
    jw_kv_opt(w, "queue_capacity", spec->queue_capacity >= 0, spec->queue_capacity);
    jw_end_object(w);
    jw_kv_bool(w, "capture_trace", spec->capture_trace);
    jw_kv_int(w, "source", spec->source); jw_kv_int(w, "destination", spec->destination);
    jw_end_object(w);
}

static void write_stat(NrJsonWriter *w, const NrStat *st) {
    jw_begin_object(w);
    jw_kv_opt(w, "mean", st->n > 0U, st->mean); jw_kv_opt(w, "stddev", st->n > 0U, st->stddev);
    jw_kv_opt(w, "ci95", st->n > 1U, st->ci95); jw_kv_opt(w, "min", st->n > 0U, st->min); jw_kv_opt(w, "max", st->n > 0U, st->max);
    jw_kv_uint(w, "n", st->n);
    jw_end_object(w);
}

static void write_experiment(NrJsonWriter *w) {
    const NrExperimentResult *r = &g.experiment;
    jw_begin_object(w);
    jw_kv_uint(w, "api_version", NR_API_VERSION);
    jw_key(w, "spec"); write_spec(w, &r->spec);
    jw_key(w, "topology"); jw_begin_object(w);
    jw_kv_string(w, "description", g.experiment_topology);
    write_topology(w, &g.experiment_network, r->spec.source, r->spec.destination);
    jw_kv_number(w, "base_rtt_ms", r->base_rtt_ms);
    jw_key(w, "route_names"); jw_begin_array(w);
    for (size_t i = 0; i < r->route.length; ++i) { const NrNode *n = nr_find_node(&g.experiment_network, r->route.nodes[i]); jw_string(w, n == NULL ? "?" : n->name); }
    jw_end_array(w);
    jw_end_object(w);
    jw_key(w, "metric_keys"); jw_begin_array(w); for (int k = 0; k < NR_AGG_COUNT; ++k) jw_string(w, nr_aggregate_name((NrAggregateMetric)k)); jw_end_array(w);
    jw_key(w, "runs"); jw_begin_array(w);
    for (size_t i = 0; i < r->run_count; ++i) {
        const NrExperimentRun *run = &r->runs[i];
        jw_begin_object(w);
        jw_kv_uint(w, "index", i); jw_kv_uint(w, "sweep_index", run->sweep_index);
        jw_kv_opt(w, "sweep_value", r->spec.sweep != NR_SWEEP_NONE, run->sweep_value);
        jw_kv_string(w, "protocol", PROTOCOLS[run->protocol]); jw_kv_uint(w, "trial", run->trial); jw_kv_uint(w, "seed", run->seed);
        jw_key(w, "metrics"); write_metrics(w, &run->metrics);
        jw_key(w, "retransmitted"); jw_begin_array(w);
        for (size_t q = 0; q < run->retransmitted_count; ++q) {
            jw_begin_object(w); jw_kv_uint(w, "seq", run->retransmitted[q].sequence); jw_kv_uint(w, "count", run->retransmitted[q].count); jw_end_object(w);
        }
        jw_end_array(w);
        jw_kv_uint(w, "retransmitted_sequences", run->retransmitted_total_sequences);
        jw_kv_bool(w, "captured", run->trace != NULL);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_key(w, "aggregates"); jw_begin_array(w);
    for (size_t i = 0; i < r->aggregate_count; ++i) {
        const NrExperimentAggregate *a = &r->aggregates[i];
        jw_begin_object(w);
        jw_kv_uint(w, "sweep_index", a->sweep_index); jw_kv_opt(w, "sweep_value", r->spec.sweep != NR_SWEEP_NONE, a->sweep_value);
        jw_kv_string(w, "protocol", PROTOCOLS[a->protocol]); jw_kv_uint(w, "runs", a->runs); jw_kv_uint(w, "completed", a->completed);
        jw_key(w, "stats"); jw_begin_object(w);
        for (int k = 0; k < NR_AGG_COUNT; ++k) { jw_key(w, nr_aggregate_name((NrAggregateMetric)k)); write_stat(w, &a->stats[k]); }
        jw_end_object(w);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_key(w, "findings"); jw_begin_array(w); for (size_t i = 0; i < r->finding_count; ++i) jw_string(w, r->findings[i]); jw_end_array(w);
    jw_key(w, "captures"); jw_begin_array(w);
    memset(&g_describer, 0, sizeof(g_describer));
    g_describer.network = &g.experiment_network;
    for (size_t i = 0; i < r->run_count; ++i) {
        const NrExperimentRun *run = &r->runs[i];
        if (run->trace == NULL) continue;
        g_describer.config.protocol = run->protocol; g_describer.config.packet_count = r->spec.config.packet_count;
        jw_begin_object(w);
        jw_kv_uint(w, "run_index", i); jw_kv_string(w, "protocol", PROTOCOLS[run->protocol]);
        jw_kv_uint(w, "sweep_index", run->sweep_index); jw_kv_uint(w, "trace_total", run->trace_count);
        jw_key(w, "trace"); jw_begin_array(w);
        for (size_t t = 0; t < run->trace_count && t < 2500U; ++t) write_trace_record(w, &g_describer, &run->trace[t], t);
        jw_end_array(w);
        jw_key(w, "faults"); jw_begin_array(w);
        for (size_t f = 0; f < run->fault_count && f < 2000U; ++f) write_fault(w, &run->faults[f], f);
        jw_end_array(w);
        jw_end_object(w);
    }
    jw_end_array(w);
    jw_end_object(w);
}

static bool run_experiment(const NrJson *req, ApiError *e) {
    NrExperimentSpec spec;
    NrNetwork *network = malloc(sizeof(*network));
    char label[160] = "";
    if (network == NULL) return set_error(e, "out of memory");
    if (!parse_experiment(req, &spec, network, label, sizeof(label), e)) { free(network); return false; }
    NrExperimentResult *result = malloc(sizeof(*result));
    if (result == NULL) { free(network); return set_error(e, "out of memory"); }
    if (!nr_experiment_run(network, &spec, result)) { set_error(e, result->error); free(result); free(network); return false; }
    if (g.has_experiment) nr_experiment_free(&g.experiment);
    g.experiment = *result; g.has_experiment = true;
    g.experiment_network = *network;
    (void)snprintf(g.experiment_topology, sizeof(g.experiment_topology), "%s", label);
    free(result); free(network);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Metric definitions (shown in the UI next to every number)
 * ------------------------------------------------------------------------------------------- */
typedef struct { const char *key, *name, *unit, *formula, *description; } MetricDefinition;
static const MetricDefinition DEFINITIONS[] = {
    {"delivery_ratio", "Packet delivery ratio", "%", "unique packets delivered / packets requested",
     "Share of the original application packets delivered (in order, at least once) to the receiving application. Retransmissions and duplicates never count twice."},
    {"goodput_bps", "Goodput", "bit/s", "unique packets delivered x payload bytes x 8 / elapsed time",
     "Useful application payload delivered per second of simulated time. Headers, ACKs, retransmissions and duplicates are excluded."},
    {"throughput_bps", "Throughput (total on the wire)", "bit/s", "(data-frame bits + ACK/NAK bits put on the wire by both endpoints) / elapsed time",
     "All bits the sender and receiver transmitted, including headers, retransmissions and acknowledgements. Each frame is counted once at its origin, not per hop."},
    {"data_throughput_bps", "Data throughput", "bit/s", "data-frame bits sent (payload + 20 B header, incl. retransmissions) / elapsed time",
     "Sender-side transmitted data rate including retransmissions."},
    {"latency_avg_ms", "Average end-to-end latency", "ms", "mean over delivered packets of (in-order delivery time - first transmission time)",
     "Measured from the packet's FIRST transmission, so it includes queueing, serialisation, propagation, every timeout/retransmission round, and (for Selective Repeat) waiting in the receiver buffer for earlier gaps to fill."},
    {"completion_time_s", "Completion time", "s", "simulation time when the last packet was acknowledged",
     "For incomplete runs this is the time at which the stopping criterion fired (retry limit or time limit); such runs are flagged completed = false."},
    {"retransmissions", "Retransmission count", "frames", "data transmissions - first transmissions",
     "Split by cause: timeout, NAK, and go-back (Go-Back-N resends of frames that followed the one being recovered). The receiver never retransmits ACKs on its own; duplicate ACKs are reported as dup_acks_sent."},
    {"retransmission_rate", "Retransmission rate", "ratio", "retransmissions / first transmissions",
     "0 means no frame was ever resent; 1 means one retransmission per original packet on average."},
    {"efficiency", "Protocol efficiency", "%", "delivered payload bits / (data-frame bits + ACK/NAK bits on the wire)",
     "How much of what was transmitted was useful payload. Different from delivery ratio: a run can deliver 100% of packets with low efficiency."},
    {"frame_efficiency", "Frame efficiency", "%", "unique packets delivered / data frames sent", "Fraction of data transmissions that produced a new delivered packet."},
    {"retx_unnecessary", "Unnecessary retransmissions", "frames", "sum over packets of (copies sent after the copy the receiver accepted)",
     "Redundant copies: the receiver had already accepted (or was going to accept) an earlier copy. Caused by premature timeouts, lost/corrupted ACKs or go-back resends."},
    {"faults", "Fault and recovery statistics", "events", "per fault type: injected, detected, recovered, unrecovered",
     "Detected: corruption caught by the checksum, or a loss the sender reacted to (timeout/NAK). Recovered: data later received correctly, or packet later acknowledged. No-impact: a lost copy of data the receiver already had."},
    {"queue_avg_occupancy", "Average queue occupancy", "frames", "time-average of waiting frames, per forwarding port (mean across used ports; worst port also reported)",
     "Only router/switch output queues are included; the endpoints' own transmit buffers are excluded."},
    {"queue_drops", "Queue drops", "frames", "frames arriving at a full output queue", "Data and ACK drops are reported separately."},
    {"congestion_decreases", "Congestion-control events", "events", "multiplicative decreases (and additive increases)",
     "rate_aimd halves the offered rate on queue overflow / high utilisation; window_aimd halves the congestion window on timeout."},
};

static void write_definitions(NrJsonWriter *w) {
    jw_begin_array(w);
    for (size_t i = 0; i < sizeof(DEFINITIONS) / sizeof(DEFINITIONS[0]); ++i) {
        jw_begin_object(w);
        jw_kv_string(w, "key", DEFINITIONS[i].key); jw_kv_string(w, "name", DEFINITIONS[i].name); jw_kv_string(w, "unit", DEFINITIONS[i].unit);
        jw_kv_string(w, "formula", DEFINITIONS[i].formula); jw_kv_string(w, "description", DEFINITIONS[i].description);
        jw_end_object(w);
    }
    jw_end_array(w);
}

/* ---------------------------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------------------------- */
static bool require_editable(ApiError *e) {
    if (sim_active()) return set_error(e, "stop or reset the running simulation before changing the topology structure");
    free_sim();
    return true;
}

static NrNode *node_arg(const NrJson *req, ApiError *e) {
    NrNode *node = nr_find_node(&g.network, json_int(req, "id", -1));
    if (node == NULL) set_error(e, "node not found");
    return node;
}

static NrLink *link_arg(const NrJson *req, ApiError *e) {
    NrLink *link = nr_find_link(&g.network, json_int(req, "id", -1));
    if (link == NULL) set_error(e, "link not found");
    return link;
}

static bool apply_link_fields(const NrJson *req, NrLink *l, ApiError *e) {
    double cost = l->cost, delay = l->delay_ms, bandwidth = l->bandwidth_mbps, loss = l->loss_probability, jitter = l->jitter_ms;
    if (!number_in(req, "cost", 0, 1e6, &cost, e) || !number_in(req, "delay_ms", 0, 60000, &delay, e) ||
        !number_in(req, "bandwidth_mbps", 0, 100000, &bandwidth, e) || !number_in(req, "loss_probability", 0, 1, &loss, e) ||
        !number_in(req, "jitter_ms", 0, 60000, &jitter, e)) return false;
    l->cost = cost; l->delay_ms = delay; l->bandwidth_mbps = bandwidth; l->loss_probability = loss; l->jitter_ms = jitter;
    return true;
}

static void fix_endpoints(void) {
    if (nr_find_node(&g.network, g.source) == NULL || nr_find_node(&g.network, g.destination) == NULL || g.source == g.destination)
        (void)nr_default_endpoints(&g.network, &g.source, &g.destination);
}

static bool dispatch(const char *cmd, const NrJson *req, NrJsonWriter *w, ApiError *e) {
    if (strcmp(cmd, "version") == 0) {
        jw_begin_object(w);
        jw_kv_string(w, "engine", "NetRescue C simulation engine"); jw_kv_uint(w, "api_version", NR_API_VERSION);
#ifdef __EMSCRIPTEN__
        jw_kv_string(w, "target", "webassembly");
#else
        jw_kv_string(w, "target", "native");
#endif
        jw_kv_string(w, "built", __DATE__ " " __TIME__);
        jw_key(w, "limits"); jw_begin_object(w);
        jw_kv_uint(w, "max_nodes", NR_MAX_NODES); jw_kv_uint(w, "max_links", NR_MAX_LINKS); jw_kv_uint(w, "max_packets", NR_ARQ_MAX_PACKETS);
        jw_kv_uint(w, "max_queue", NR_ARQ_MAX_QUEUE); jw_kv_uint(w, "max_rules", NR_ARQ_MAX_RULES); jw_kv_uint(w, "max_trials", NR_EXP_MAX_TRIALS);
        jw_kv_uint(w, "max_sweep_values", NR_EXP_MAX_SWEEP); jw_kv_uint(w, "header_bytes", NR_ARQ_HEADER_BYTES); jw_kv_uint(w, "ack_bytes", NR_ARQ_ACK_BYTES);
        jw_end_object(w);
        jw_end_object(w);
        return true;
    }
    if (strcmp(cmd, "metric_definitions") == 0) { write_definitions(w); return true; }
    if (strcmp(cmd, "get_network") == 0) { write_network(w); return true; }
    if (strcmp(cmd, "load_demo") == 0) {
        if (!require_editable(e)) return false;
        (void)nr_load_demo(&g.network);
        (void)nr_default_endpoints(&g.network, &g.source, &g.destination);
        write_network(w); return true;
    }
    if (strcmp(cmd, "generate_topology") == 0) {
        if (!require_editable(e)) return false;
        const int type = lookup(TOPOLOGIES, NR_TOPO_COUNT, nr_json_string(nr_json_get(req, "type"), ""));
        if (type < 0) return set_error(e, "type must be line, bus, star, ring, tree, mesh, hybrid or random");
        const int nodes = json_int(req, "nodes", 8);
        NrNetwork *candidate = malloc(sizeof(*candidate));
        if (candidate == NULL) return set_error(e, "out of memory");
        if (!nr_generate_topology(candidate, (NrTopologyType)type, (size_t)(nodes < 0 ? 0 : nodes), (unsigned)json_int(req, "seed", 1))) {
            free(candidate); return set_error(e, "could not generate topology (2-64 nodes; a full mesh allows at most 23 nodes)");
        }
        g.network = *candidate; free(candidate);
        (void)nr_default_endpoints(&g.network, &g.source, &g.destination);
        write_network(w); return true;
    }
    if (strcmp(cmd, "add_node") == 0) {
        if (!require_editable(e)) return false;
        const int type = lookup(NODE_TYPES, 5, nr_json_string(nr_json_get(req, "type"), "router"));
        if (type < 0) return set_error(e, "node type must be host, router, switch, server or access_point");
        char name[NR_NAME_LEN];
        (void)snprintf(name, sizeof(name), "%s", nr_json_string(nr_json_get(req, "name"), ""));
        if (name[0] == '\0') { static const char *prefix[] = {"H", "R", "S", "SV", "AP"}; (void)snprintf(name, sizeof(name), "%s%u", prefix[type], g.network.next_node_id); }
        for (char *c = name; *c != '\0'; ++c) if (*c == ' ') *c = '_';
        const double x = fmin(1.0, fmax(0.0, nr_json_number(nr_json_get(req, "x"), 0.5))), y = fmin(1.0, fmax(0.0, nr_json_number(nr_json_get(req, "y"), 0.5)));
        const int id = nr_add_node(&g.network, name, (NrNodeType)type, (float)x, (float)y);
        if (id < 0) return set_error(e, "could not add node (limit 64 nodes)");
        if (has_number(req, "queue_capacity")) nr_find_node(&g.network, id)->queue_capacity = (unsigned)fmax(0.0, fmin(NR_ARQ_MAX_QUEUE, nr_json_number(nr_json_get(req, "queue_capacity"), 10)));
        fix_endpoints();
        write_network(w); return true;
    }
    if (strcmp(cmd, "update_node") == 0) {
        NrNode *node = node_arg(req, e);
        if (node == NULL) return false;
        const bool structural = nr_json_get(req, "type") != NULL || nr_json_get(req, "name") != NULL;
        if (structural && sim_active()) return set_error(e, "stop or reset the simulation before renaming or retyping nodes");
        if (nr_json_get(req, "name") != NULL) {
            const char *name = nr_json_string(nr_json_get(req, "name"), "");
            if (name[0] == '\0') return set_error(e, "name must not be empty");
            (void)snprintf(node->name, sizeof(node->name), "%s", name);
            for (char *c = node->name; *c != '\0'; ++c) if (*c == ' ') *c = '_';
        }
        if (nr_json_get(req, "type") != NULL) {
            const int type = lookup(NODE_TYPES, 5, nr_json_string(nr_json_get(req, "type"), ""));
            if (type < 0) return set_error(e, "node type must be host, router, switch, server or access_point");
            node->type = (NrNodeType)type;
        }
        if (has_number(req, "x")) node->x = (float)fmin(1.0, fmax(0.0, nr_json_number(nr_json_get(req, "x"), node->x)));
        if (has_number(req, "y")) node->y = (float)fmin(1.0, fmax(0.0, nr_json_number(nr_json_get(req, "y"), node->y)));
        double q = node->queue_capacity;
        if (!number_in(req, "queue_capacity", 0, NR_ARQ_MAX_QUEUE, &q, e)) return false;
        node->queue_capacity = (unsigned)q;
        if (nr_json_get(req, "active") != NULL) {
            (void)nr_set_node_active(&g.network, node->id, nr_json_bool(nr_json_get(req, "active"), true));
            if (sim_active()) nr_arq_topology_changed(&g.sim);
        }
        write_network(w); return true;
    }
    if (strcmp(cmd, "remove_node") == 0) {
        if (!require_editable(e)) return false;
        if (!nr_remove_node(&g.network, json_int(req, "id", -1))) return set_error(e, "node not found");
        fix_endpoints();
        write_network(w); return true;
    }
    if (strcmp(cmd, "add_link") == 0) {
        if (!require_editable(e)) return false;
        const int from = json_int(req, "from", -1), to = json_int(req, "to", -1);
        double cost = 1.0, delay = 10.0, bandwidth = 10.0;
        if (!number_in(req, "cost", 0, 1e6, &cost, e) || !number_in(req, "delay_ms", 0, 60000, &delay, e) || !number_in(req, "bandwidth_mbps", 0, 100000, &bandwidth, e)) return false;
        const int id = nr_add_link(&g.network, from, to, nr_json_bool(nr_json_get(req, "bidirectional"), true), cost, delay, bandwidth);
        if (id < 0) return set_error(e, "could not add link (nodes must exist and differ, no duplicate links, limit 256 links)");
        if (!apply_link_fields(req, nr_find_link(&g.network, id), e)) { (void)nr_remove_link(&g.network, id); return false; }
        write_network(w); return true;
    }
    if (strcmp(cmd, "update_link") == 0) {
        NrLink *link = link_arg(req, e);
        if (link == NULL) return false;
        if (nr_json_get(req, "bidirectional") != NULL) {
            if (sim_active()) return set_error(e, "stop or reset the simulation before changing link direction");
            link->bidirectional = nr_json_bool(nr_json_get(req, "bidirectional"), true);
        }
        if (!apply_link_fields(req, link, e)) return false;
        if (nr_json_get(req, "active") != NULL) {
            (void)nr_set_link_active(&g.network, link->id, nr_json_bool(nr_json_get(req, "active"), true));
            if (sim_active()) nr_arq_topology_changed(&g.sim);
        }
        write_network(w); return true;
    }
    if (strcmp(cmd, "remove_link") == 0) {
        if (!require_editable(e)) return false;
        if (!nr_remove_link(&g.network, json_int(req, "id", -1))) return set_error(e, "link not found");
        write_network(w); return true;
    }
    if (strcmp(cmd, "set_all_links") == 0) {
        if (sim_active()) return set_error(e, "stop or reset the simulation first");
        for (size_t j = 0; j < g.network.link_count; ++j) if (!apply_link_fields(req, &g.network.links[j], e)) return false;
        write_network(w); return true;
    }
    if (strcmp(cmd, "set_all_queues") == 0) {
        if (sim_active()) return set_error(e, "stop or reset the simulation first");
        double q = 10; if (!number_in(req, "capacity", 0, NR_ARQ_MAX_QUEUE, &q, e)) return false;
        for (size_t i = 0; i < g.network.node_count; ++i) g.network.nodes[i].queue_capacity = (unsigned)q;
        write_network(w); return true;
    }
    if (strcmp(cmd, "set_endpoints") == 0) {
        if (sim_active()) return set_error(e, "stop or reset the simulation before changing endpoints");
        const int source = json_int(req, "source", g.source), destination = json_int(req, "destination", g.destination);
        if (nr_find_node(&g.network, source) == NULL || nr_find_node(&g.network, destination) == NULL) return set_error(e, "endpoint node not found");
        if (source == destination) return set_error(e, "source and destination must differ");
        free_sim();
        g.source = source; g.destination = destination;
        write_network(w); return true;
    }
    if (strcmp(cmd, "set_cost_mode") == 0) {
        if (sim_active()) return set_error(e, "stop or reset the simulation first");
        const int mode = lookup(COST_MODES, 3, nr_json_string(nr_json_get(req, "mode"), ""));
        if (mode < 0) return set_error(e, "mode must be link, delay or composite");
        g.network.cost_mode = (NrCostMode)mode;
        write_network(w); return true;
    }
    if (strcmp(cmd, "export_topology") == 0) {
        char *text = nr_topology_to_text(&g.network);
        if (text == NULL) return set_error(e, "out of memory");
        jw_begin_object(w); jw_kv_string(w, "format", "NETRESCUE 2"); jw_kv_string(w, "text", text); jw_end_object(w);
        free(text); return true;
    }
    if (strcmp(cmd, "import_topology") == 0) {
        if (!require_editable(e)) return false;
        NrNetwork *candidate = malloc(sizeof(*candidate));
        if (candidate == NULL) return set_error(e, "out of memory");
        if (!nr_topology_from_text(candidate, nr_json_string(nr_json_get(req, "text"), ""))) { free(candidate); return set_error(e, "invalid topology file (expected a NETRESCUE 1 or 2 file)"); }
        g.network = *candidate; free(candidate);
        (void)nr_default_endpoints(&g.network, &g.source, &g.destination);
        write_network(w); return true;
    }
    if (strcmp(cmd, "get_config") == 0) {
        jw_begin_object(w); jw_key(w, "config"); write_config(w, &g.config); jw_key(w, "scenario"); write_scenario(w, &g.scenario); jw_end_object(w);
        return true;
    }
    if (strcmp(cmd, "configure") == 0 || strcmp(cmd, "sim_start") == 0) {
        NrArqConfig config = g.config; NrFaultScenario scenario = g.scenario;
        if (!parse_config(nr_json_get(req, "config"), &config, e)) return false;
        if (nr_json_get(req, "scenario") != NULL && !parse_scenario(nr_json_get(req, "scenario"), &scenario, e)) return false;
        g.config = config; g.scenario = scenario;
        if (strcmp(cmd, "configure") == 0) {
            jw_begin_object(w); jw_key(w, "config"); write_config(w, &g.config); jw_key(w, "scenario"); write_scenario(w, &g.scenario); jw_end_object(w);
            return true;
        }
        free_sim();
        NrIssue issues[NR_MAX_ISSUES];
        const size_t count = nr_validate_network(&g.network, g.source, g.destination, issues);
        for (size_t i = 0; i < count; ++i) if (issues[i].severity == NR_ISSUE_ERROR) return set_error(e, issues[i].message);
        if (!nr_arq_start_scenario(&g.sim, &g.network, &g.config, &g.scenario, g.source, g.destination))
            return set_error(e, "the engine rejected the configuration (check the window, timeout and probabilities)");
        g.sim_allocated = true;
        write_sim_state(w, req); return true;
    }
    if (strcmp(cmd, "sim_pause") == 0) { nr_arq_pause(&g.sim); write_sim_state(w, req); return true; }
    if (strcmp(cmd, "sim_resume") == 0) { nr_arq_resume(&g.sim); write_sim_state(w, req); return true; }
    if (strcmp(cmd, "sim_stop") == 0) { nr_arq_stop(&g.sim); write_sim_state(w, req); return true; }
    if (strcmp(cmd, "sim_reset") == 0) { free_sim(); write_sim_state(w, req); return true; }
    if (strcmp(cmd, "sim_step") == 0) {
        double dt = 0.0;
        if (!number_in(req, "dt", 0, 1000, &dt, e)) return false;
        if (g.sim_allocated && dt > 0.0) nr_arq_step(&g.sim, dt);
        write_sim_state(w, req); return true;
    }
    if (strcmp(cmd, "sim_step_event") == 0) {
        if (g.sim_allocated) (void)nr_arq_step_event(&g.sim);
        write_sim_state(w, req); return true;
    }
    if (strcmp(cmd, "sim_state") == 0) { write_sim_state(w, req); return true; }
    if (strcmp(cmd, "sim_export") == 0) {
        if (!g.sim_allocated) return set_error(e, "no simulation to export");
        char line[512];
        size_t capacity = 4096U, length = 0U;
        char *text = malloc(capacity);
        if (text == NULL) return set_error(e, "out of memory");
        length += (size_t)snprintf(text, capacity, "index,time_s,type,seq,ack,attempt,frame,node,link,detail,text\n");
        for (size_t i = 0; i < g.sim.trace_count; ++i) {
            const NrArqTrace *r = &g.sim.trace[i];
            char description[NR_EVENT_LEN]; nr_arq_trace_describe(&g.sim, r, description, sizeof(description));
            for (char *c = description; *c != '\0'; ++c) if (*c == '"') *c = '\'';
            const int n = snprintf(line, sizeof(line), "%zu,%.9f,%s,%d,%d,%d,%u,%d,%d,%d,\"%s\"\n", i, r->time, nr_arq_trace_name(r->type), r->sequence,
                                   r->ack, r->attempt, r->frame_id, r->node_id, r->link_id, r->detail, description);
            if (n <= 0) continue;
            if (length + (size_t)n + 1U > capacity) {
                char *grown = realloc(text, capacity * 2U + (size_t)n);
                if (grown == NULL) { free(text); return set_error(e, "out of memory"); }
                text = grown; capacity = capacity * 2U + (size_t)n;
            }
            memcpy(text + length, line, (size_t)n); length += (size_t)n;
        }
        text[length] = '\0';
        jw_begin_object(w);
        jw_kv_string(w, "trace_csv", text);
        free(text);
        jw_key(w, "faults"); jw_begin_array(w);
        for (size_t i = 0; i < g.sim.fault_count; ++i) write_fault(w, &g.sim.faults[i], i);
        jw_end_array(w);
        NrArqMetrics m; nr_arq_compute_metrics(&g.sim, &m);
        jw_key(w, "metrics"); write_metrics(w, &m);
        jw_key(w, "config"); write_config(w, &g.sim.config);
        jw_key(w, "scenario"); write_scenario(w, &g.sim.scenario);
        jw_end_object(w);
        return true;
    }
    if (strcmp(cmd, "list_presets") == 0) {
        jw_begin_array(w);
        for (size_t i = 0; i < nr_preset_count(); ++i) {
            const NrPresetInfo *p = nr_preset_info(i);
            jw_begin_object(w);
            jw_kv_string(w, "id", p->id); jw_kv_string(w, "title", p->title); jw_kv_string(w, "purpose", p->purpose);
            jw_kv_string(w, "settings", p->settings); jw_kv_string(w, "what_to_look_for", p->what_to_look_for);
            jw_end_object(w);
        }
        jw_end_array(w);
        return true;
    }
    if (strcmp(cmd, "preset_spec") == 0) {
        const int index = nr_preset_find(nr_json_string(nr_json_get(req, "id"), ""));
        if (index < 0) return set_error(e, "unknown preset id");
        NrNetwork *n = malloc(sizeof(*n)); NrExperimentSpec spec;
        if (n == NULL || !nr_preset_build((size_t)index, n, &spec)) { free(n); return set_error(e, "could not build preset"); }
        jw_begin_object(w);
        jw_kv_string(w, "id", nr_preset_info((size_t)index)->id);
        jw_key(w, "spec"); write_spec(w, &spec);
        jw_key(w, "topology"); jw_begin_object(w); write_topology(w, n, spec.source, spec.destination); jw_end_object(w);
        jw_end_object(w);
        free(n);
        return true;
    }
    if (strcmp(cmd, "run_experiment") == 0) {
        if (!run_experiment(req, e)) return false;
        write_experiment(w); return true;
    }
    if (strcmp(cmd, "run_preset") == 0) {
        const char *id = nr_json_string(nr_json_get(req, "id"), "");
        if (nr_preset_find(id) < 0) return set_error(e, "unknown preset id");
        /* Build {"topology":{"kind":"preset","id":...}} by hand to reuse the generic path. */
        char request[160]; (void)snprintf(request, sizeof(request), "{\"topology\":{\"kind\":\"preset\",\"id\":\"%s\"}}", id);
        char error[64]; NrJson *built = nr_json_parse(request, error, sizeof(error));
        if (built == NULL) return set_error(e, "internal error");
        const bool ok = run_experiment(built, e);
        nr_json_free(built);
        if (!ok) return false;
        write_experiment(w); return true;
    }
    if (strcmp(cmd, "get_experiment") == 0) {
        if (!g.has_experiment) return set_error(e, "no experiment has been run yet");
        write_experiment(w); return true;
    }
    if (strcmp(cmd, "export_experiment_csv") == 0) {
        if (!g.has_experiment) return set_error(e, "no experiment has been run yet");
        char *csv = nr_experiment_csv(&g.experiment);
        if (csv == NULL) return set_error(e, "out of memory");
        jw_begin_object(w); jw_kv_string(w, "csv", csv); jw_end_object(w);
        free(csv); return true;
    }
    return set_error(e, "unknown command");
}

const char *nr_api_call(const char *request_json) {
    ensure_init();
    free(g.response); g.response = NULL;
    NrJsonWriter w; jw_init(&w);
    ApiError e; e.message[0] = '\0';
    char parse_error[128];
    NrJson *req = nr_json_parse(request_json, parse_error, sizeof(parse_error));
    bool ok = false;
    NrJsonWriter body; jw_init(&body);
    if (req == NULL) (void)snprintf(e.message, sizeof(e.message), "invalid request JSON: %s", parse_error);
    else if (req->type != NR_JSON_OBJECT) (void)snprintf(e.message, sizeof(e.message), "request must be a JSON object");
    else {
        const char *cmd = nr_json_string(nr_json_get(req, "cmd"), NULL);
        if (cmd == NULL) (void)snprintf(e.message, sizeof(e.message), "missing 'cmd'");
        else ok = dispatch(cmd, req, &body, &e);
    }
    nr_json_free(req);
    jw_begin_object(&w);
    jw_kv_bool(&w, "ok", ok && body.ok);
    jw_kv_uint(&w, "api_version", NR_API_VERSION);
    if (ok && body.ok) { jw_key(&w, "result"); jw_raw(&w, body.data); }
    else jw_kv_string(&w, "error", e.message[0] != '\0' ? e.message : "out of memory");
    jw_end_object(&w);
    jw_free(&body);
    g.response = jw_take(&w);
    return g.response != NULL ? g.response : "{\"ok\":false,\"api_version\":1,\"error\":\"out of memory\"}";
}
