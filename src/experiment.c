#include "experiment.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOTAL_RUNS 4000U

static const char *SWEEP_NAMES[NR_SWEEP_COUNT] = {
    "none", "loss_probability", "corruption_probability", "ack_loss_probability", "ack_corruption_probability",
    "timeout_s", "window_size", "packets_per_second", "queue_capacity", "link_delay_ms", "link_bandwidth_mbps",
    "link_jitter_ms", "congestion_mode", "payload_bytes"};

static const char *AGG_NAMES[NR_AGG_COUNT] = {
    "delivery_ratio", "goodput_bps", "throughput_bps", "data_throughput_bps", "latency_avg_ms", "latency_max_ms",
    "completion_time_s", "retransmissions", "retransmission_rate", "efficiency", "frame_efficiency", "timeouts",
    "retx_unnecessary", "retx_goback", "data_transmissions", "acks_sent", "faults_injected", "faults_recovered",
    "faults_unrecovered", "data_losses", "corruptions_detected", "ack_losses", "queue_avg_occupancy",
    "queue_max_occupancy", "queue_drops", "congestion_decreases", "packets_undelivered", "duplicates_received"};

const char *nr_sweep_name(NrSweepParameter p) { return p >= NR_SWEEP_NONE && p < NR_SWEEP_COUNT ? SWEEP_NAMES[p] : "unknown"; }
const char *nr_aggregate_name(NrAggregateMetric m) { return m >= 0 && m < NR_AGG_COUNT ? AGG_NAMES[m] : "unknown"; }

bool nr_sweep_from_name(const char *name, NrSweepParameter *p) {
    if (name == NULL || p == NULL) return false;
    for (int i = 0; i < NR_SWEEP_COUNT; ++i) if (strcmp(name, SWEEP_NAMES[i]) == 0) { *p = (NrSweepParameter)i; return true; }
    return false;
}

double nr_t_critical_95(unsigned df) {
    static const double table[] = {0.0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160,
                                   2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052,
                                   2.048, 2.045, 2.042};
    if (df == 0U) return 0.0;
    if (df <= 30U) return table[df];
    if (df <= 40U) return 2.021;
    if (df <= 60U) return 2.000;
    if (df <= 120U) return 1.980;
    return 1.960;
}

void nr_experiment_default_spec(NrExperimentSpec *spec) {
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
    nr_arq_default_config(&spec->config);
    spec->config.packet_count = 100U; spec->config.window_size = 8U;
    spec->protocol_mask = 7U; spec->trials = 1U; spec->seed_base = 1000U; spec->seed_stride = 7919U;
    spec->link_delay_ms = -1.0; spec->link_bandwidth_mbps = -1.0; spec->link_jitter_ms = -1.0; spec->queue_capacity = -1;
    spec->source = -1; spec->destination = -1;
}

double nr_route_base_rtt_ms(const NrNetwork *n, const NrRoute *forward, const NrRoute *reverse, unsigned payload_bytes) {
    double total = 0.0;
    const NrRoute *routes[2] = {forward, reverse};
    for (int r = 0; r < 2; ++r) {
        const double bits = (r == 0 ? (double)(payload_bytes + NR_ARQ_HEADER_BYTES) : (double)NR_ARQ_ACK_BYTES) * 8.0;
        for (size_t i = 0; routes[r] != NULL && i + 1U < routes[r]->length; ++i) {
            const int link = nr_find_link_between(n, routes[r]->nodes[i], routes[r]->nodes[i + 1U], NULL);
            if (link < 0) return NAN;
            const NrLink *l = &n->links[link];
            total += l->delay_ms + (l->bandwidth_mbps > 0.0 ? bits / (l->bandwidth_mbps * 1000.0) : 0.0);
        }
    }
    return total;
}

static void apply_network_value(NrNetwork *n, NrSweepParameter p, double v) {
    for (size_t j = 0; j < n->link_count; ++j) {
        if (p == NR_SWEEP_DELAY) n->links[j].delay_ms = v;
        else if (p == NR_SWEEP_BANDWIDTH) n->links[j].bandwidth_mbps = v;
        else if (p == NR_SWEEP_JITTER) n->links[j].jitter_ms = v;
    }
    if (p == NR_SWEEP_QUEUE_CAPACITY) for (size_t i = 0; i < n->node_count; ++i) n->nodes[i].queue_capacity = (unsigned)v;
}

static void apply_config_value(NrArqConfig *c, NrSweepParameter p, double v) {
    switch (p) {
    case NR_SWEEP_LOSS: c->loss_probability = v; break;
    case NR_SWEEP_CORRUPTION: c->corruption_probability = v; break;
    case NR_SWEEP_ACK_LOSS: c->ack_loss_probability = v; break;
    case NR_SWEEP_ACK_CORRUPTION: c->ack_corruption_probability = v; break;
    case NR_SWEEP_TIMEOUT: c->timeout_seconds = v; break;
    case NR_SWEEP_WINDOW: c->window_size = (unsigned)v; break;
    case NR_SWEEP_RATE: c->packets_per_second = v; break;
    case NR_SWEEP_CONGESTION_MODE: c->congestion_mode = (NrCongestionMode)(int)v; c->congestion_control = false; break;
    case NR_SWEEP_PAYLOAD: c->payload_bytes = (unsigned)v; break;
    default: break;
    }
}

static bool metric_value(const NrArqMetrics *m, NrAggregateMetric k, double *out) {
    switch (k) {
    case NR_AGG_DELIVERY_RATIO: *out = m->delivery_ratio; return m->has_delivery_ratio;
    case NR_AGG_GOODPUT: *out = m->goodput_bps; return m->has_goodput;
    case NR_AGG_THROUGHPUT: *out = m->throughput_bps; return m->has_goodput;
    case NR_AGG_DATA_THROUGHPUT: *out = m->data_throughput_bps; return m->has_goodput;
    case NR_AGG_LATENCY_AVG: *out = m->latency_avg_ms; return m->has_latency;
    case NR_AGG_LATENCY_MAX: *out = m->latency_max_ms; return m->has_latency;
    case NR_AGG_COMPLETION_TIME: *out = m->elapsed_seconds; return true;
    case NR_AGG_RETRANSMISSIONS: *out = (double)m->retransmissions; return true;
    case NR_AGG_RETX_RATE: *out = m->retransmission_rate; return m->has_retx_rate;
    case NR_AGG_EFFICIENCY: *out = m->efficiency; return m->has_efficiency;
    case NR_AGG_FRAME_EFFICIENCY: *out = m->frame_efficiency; return m->data_transmissions > 0U;
    case NR_AGG_TIMEOUTS: *out = (double)m->timeouts; return true;
    case NR_AGG_RETX_UNNECESSARY: *out = (double)m->retx_unnecessary; return true;
    case NR_AGG_RETX_GOBACK: *out = (double)m->retx_goback; return true;
    case NR_AGG_DATA_TRANSMISSIONS: *out = (double)m->data_transmissions; return true;
    case NR_AGG_ACKS_SENT: *out = (double)(m->acks_sent + m->naks_sent); return true;
    case NR_AGG_FAULTS_INJECTED: *out = (double)m->faults_total; return true;
    case NR_AGG_FAULTS_RECOVERED: *out = (double)m->faults_recovered; return true;
    case NR_AGG_FAULTS_UNRECOVERED: *out = (double)m->faults_unrecovered; return true;
    case NR_AGG_DATA_LOSSES: *out = (double)m->faults[NR_FAULT_DATA_LOSS].injected; return true;
    case NR_AGG_CORRUPTIONS_DETECTED: *out = (double)m->faults[NR_FAULT_DATA_CORRUPTION].detected; return true;
    case NR_AGG_ACK_LOSSES: *out = (double)m->faults[NR_FAULT_ACK_LOSS].injected; return true;
    case NR_AGG_QUEUE_AVG: *out = m->queue_max_avg_occupancy; return true;
    case NR_AGG_QUEUE_MAX: *out = (double)m->queue_max_occupancy; return true;
    case NR_AGG_QUEUE_DROPS: *out = (double)m->queue_drops; return true;
    case NR_AGG_CONGESTION_DECREASES: *out = (double)m->congestion_decreases; return true;
    case NR_AGG_UNDELIVERED: *out = (double)m->packets_undelivered; return true;
    case NR_AGG_DUPLICATES: *out = (double)m->duplicates_received; return true;
    default: return false;
    }
}

static NrStat compute_stat(const double *values, unsigned n) {
    NrStat st; memset(&st, 0, sizeof(st));
    st.n = n;
    if (n == 0U) { st.mean = st.stddev = st.ci95 = st.min = st.max = NAN; return st; }
    double sum = 0.0; st.min = values[0]; st.max = values[0];
    for (unsigned i = 0; i < n; ++i) { sum += values[i]; if (values[i] < st.min) st.min = values[i]; if (values[i] > st.max) st.max = values[i]; }
    st.mean = sum / (double)n;
    if (n > 1U) {
        double sq = 0.0;
        for (unsigned i = 0; i < n; ++i) sq += (values[i] - st.mean) * (values[i] - st.mean);
        st.stddev = sqrt(sq / (double)(n - 1U));
        st.ci95 = nr_t_critical_95(n - 1U) * st.stddev / sqrt((double)n);
    } else { st.stddev = 0.0; st.ci95 = NAN; }
    return st;
}

static void collect_evidence(const NrArqSimulation *s, NrExperimentRun *run) {
    for (size_t i = 0; i < s->config.packet_count; ++i) {
        if (s->frames[i].attempts <= 1U) continue;
        ++run->retransmitted_total_sequences;
        if (run->retransmitted_count < NR_EXP_RETX_EVIDENCE) {
            run->retransmitted[run->retransmitted_count].sequence = (unsigned)i;
            run->retransmitted[run->retransmitted_count].count = s->frames[i].attempts - 1U;
            ++run->retransmitted_count;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 * Evidence-based findings, generated only from the measured aggregates
 * ------------------------------------------------------------------------------------------- */
static void add_finding(NrExperimentResult *r, const char *format, ...) {
    if (r->finding_count >= NR_EXP_MAX_FINDINGS) return;
    va_list args; va_start(args, format);
    (void)vsnprintf(r->findings[r->finding_count], NR_EXP_FINDING_LEN, format, args);
    va_end(args);
    ++r->finding_count;
}

static const NrExperimentAggregate *find_aggregate(const NrExperimentResult *r, unsigned si, NrArqProtocol p) {
    for (size_t i = 0; i < r->aggregate_count; ++i) if (r->aggregates[i].sweep_index == si && r->aggregates[i].protocol == p) return &r->aggregates[i];
    return NULL;
}

static void format_value(char *b, size_t n, NrAggregateMetric k, double v) {
    if (!isfinite(v)) { (void)snprintf(b, n, "n/a"); return; }
    switch (k) {
    case NR_AGG_GOODPUT: case NR_AGG_THROUGHPUT: case NR_AGG_DATA_THROUGHPUT:
        if (v >= 1e6) (void)snprintf(b, n, "%.3f Mbit/s", v / 1e6); else (void)snprintf(b, n, "%.1f kbit/s", v / 1e3); break;
    case NR_AGG_LATENCY_AVG: case NR_AGG_LATENCY_MAX: (void)snprintf(b, n, "%.1f ms", v); break;
    case NR_AGG_COMPLETION_TIME: (void)snprintf(b, n, "%.3f s", v); break;
    case NR_AGG_DELIVERY_RATIO: case NR_AGG_EFFICIENCY: case NR_AGG_FRAME_EFFICIENCY: case NR_AGG_RETX_RATE: (void)snprintf(b, n, "%.1f%%", v * 100.0); break;
    default: (void)snprintf(b, n, "%.1f", v); break;
    }
}

static void point_label(const NrExperimentResult *r, unsigned si, char *b, size_t n) {
    if (r->spec.sweep == NR_SWEEP_NONE || r->spec.sweep_count == 0U) { (void)snprintf(b, n, "This run"); return; }
    if (r->spec.sweep == NR_SWEEP_CONGESTION_MODE) (void)snprintf(b, n, "With congestion control '%s'", nr_congestion_mode_name((NrCongestionMode)(int)r->spec.sweep_values[si]));
    else (void)snprintf(b, n, "At %s = %g", nr_sweep_name(r->spec.sweep), r->spec.sweep_values[si]);
}

static void ranking_finding(NrExperimentResult *r, unsigned si, NrAggregateMetric k, bool higher_better, const char *label) {
    const NrExperimentAggregate *list[NR_ARQ_PROTOCOL_COUNT]; size_t count = 0U;
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
        const NrExperimentAggregate *a = find_aggregate(r, si, (NrArqProtocol)p);
        if (a != NULL && a->stats[k].n > 0U && isfinite(a->stats[k].mean)) list[count++] = a;
    }
    if (count < 2U) return;
    for (size_t i = 0; i < count; ++i) for (size_t j = i + 1U; j < count; ++j) {
        const bool swap = higher_better ? list[j]->stats[k].mean > list[i]->stats[k].mean : list[j]->stats[k].mean < list[i]->stats[k].mean;
        if (swap) { const NrExperimentAggregate *t = list[i]; list[i] = list[j]; list[j] = t; }
    }
    char text[NR_EXP_FINDING_LEN] = "", where[96], value[48], previous[48] = "";
    point_label(r, si, where, sizeof(where));
    size_t used = (size_t)snprintf(text, sizeof(text), "%s, %s: ", where, label);
    for (size_t i = 0; i < count && used < sizeof(text); ++i) {
        format_value(value, sizeof(value), k, list[i]->stats[k].mean);
        /* Values that are equal at the displayed precision are reported as ties. */
        const char *sep = i == 0U ? "" : (strcmp(value, previous) == 0 ? " = " : (higher_better ? " > " : " < "));
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s %s", sep, nr_arq_protocol_name(list[i]->protocol), value);
        (void)snprintf(previous, sizeof(previous), "%s", value);
    }
    const NrStat *a = &list[0]->stats[k], *b = &list[1]->stats[k];
    if (used < sizeof(text) && a->n > 1U) {
        if (a->stddev == 0.0 && b->stddev == 0.0)
            (void)snprintf(text + used, sizeof(text) - used, " (identical in all %u runs)", a->n);
        else if (b->n > 1U && isfinite(a->ci95) && isfinite(b->ci95) && fabs(a->mean - b->mean) <= a->ci95 + b->ci95)
            (void)snprintf(text + used, sizeof(text) - used, " (mean of %u runs; the top two overlap within their 95%% confidence intervals)", a->n);
        else
            (void)snprintf(text + used, sizeof(text) - used, " (mean of %u runs; 95%% confidence intervals do not overlap)", a->n);
    }
    add_finding(r, "%s", text);
}

static void generate_findings(NrExperimentResult *r) {
    const unsigned points = r->spec.sweep_count == 0U ? 1U : (unsigned)r->spec.sweep_count;
    char where[96], v1[48], v2[48];
    unsigned incomplete_total = 0U;
    for (size_t i = 0; i < r->aggregate_count; ++i) incomplete_total += r->aggregates[i].runs - r->aggregates[i].completed;
    if (incomplete_total == 0U) add_finding(r, "All %zu runs completed: every original packet was delivered and acknowledged before the stopping criteria.", r->run_count);
    for (size_t i = 0; i < r->aggregate_count; ++i) {
        const NrExperimentAggregate *a = &r->aggregates[i];
        if (a->completed == a->runs) continue;
        const char *reason = "unknown";
        for (size_t j = 0; j < r->run_count; ++j)
            if (r->runs[j].sweep_index == a->sweep_index && r->runs[j].protocol == a->protocol && !r->runs[j].metrics.completed) { reason = nr_arq_stop_reason_name(r->runs[j].metrics.stop_reason); break; }
        point_label(r, a->sweep_index, where, sizeof(where));
        format_value(v1, sizeof(v1), NR_AGG_DELIVERY_RATIO, a->stats[NR_AGG_DELIVERY_RATIO].mean);
        add_finding(r, "%s, %u of %u %s runs did NOT complete (first stop reason: %s); mean delivery ratio %s. Incomplete runs are included in the averages.",
                    where, a->runs - a->completed, a->runs, nr_arq_protocol_name(a->protocol), reason, v1);
    }

    for (unsigned si = 0; si < points; ++si) {
        if (points > 1U && si != 0U && si + 1U != points) continue; /* rank only at the sweep end points */
        ranking_finding(r, si, NR_AGG_GOODPUT, true, "mean goodput");
        ranking_finding(r, si, NR_AGG_LATENCY_AVG, false, "mean end-to-end latency");
        ranking_finding(r, si, NR_AGG_COMPLETION_TIME, false, "completion time");
        ranking_finding(r, si, NR_AGG_RETRANSMISSIONS, false, "data retransmissions");
        ranking_finding(r, si, NR_AGG_EFFICIENCY, true, "protocol efficiency");
    }

    /* Mechanism evidence: Go-Back-N versus Selective Repeat retransmission behaviour. */
    for (unsigned si = 0; si < points; ++si) {
        const NrExperimentAggregate *g = find_aggregate(r, si, NR_ARQ_GO_BACK_N), *s = find_aggregate(r, si, NR_ARQ_SELECTIVE_REPEAT);
        if (g == NULL || s == NULL) continue;
        if (points > 1U && si + 1U != points) continue;
        point_label(r, si, where, sizeof(where));
        const double gr = g->stats[NR_AGG_RETRANSMISSIONS].mean, gb = g->stats[NR_AGG_RETX_GOBACK].mean, sr = s->stats[NR_AGG_RETRANSMISSIONS].mean;
        if (gr > 0.0 || sr > 0.0)
            add_finding(r, "%s, Go-Back-N made %.1f retransmissions per run, %.1f of them go-back resends of frames that followed a missing one; Selective Repeat made %.1f, resending only frames whose own timer expired or that were NAKed.",
                        where, gr, gb, sr);
        if (r->spec.trials == 1U) {
            for (size_t j = 0; j < r->run_count; ++j) {
                const NrExperimentRun *run = &r->runs[j];
                if (run->sweep_index != si || run->retransmitted_total_sequences == 0U || run->retransmitted_total_sequences > 24U) continue;
                char list[200] = ""; size_t used = 0U;
                for (size_t q = 0; q < run->retransmitted_count && used < sizeof(list); ++q)
                    used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%u%s", q == 0U ? "" : ", ", run->retransmitted[q].sequence,
                                             run->retransmitted[q].count > 1U ? "(x2+)" : "");
                add_finding(r, "Trace evidence (%s): %s retransmitted sequence numbers {%s}.", where, nr_arq_protocol_name(run->protocol), list);
            }
        }
    }

    /* Timeout and unnecessary retransmissions. */
    for (unsigned si = 0; si < points; ++si) {
        for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
            const NrExperimentAggregate *a = find_aggregate(r, si, (NrArqProtocol)p);
            if (a == NULL || !(a->stats[NR_AGG_RETX_UNNECESSARY].mean > 0.0)) continue;
            NrArqConfig c = r->spec.config;
            apply_config_value(&c, r->spec.sweep, r->spec.sweep_count > 0U ? r->spec.sweep_values[si] : 0.0);
            point_label(r, si, where, sizeof(where));
            add_finding(r, "%s, %s sent %.1f unnecessary retransmissions per run (copies sent after the copy the receiver accepted). Timeout %.0f ms vs no-load RTT %.1f ms%s.",
                        where, nr_arq_protocol_name((NrArqProtocol)p), a->stats[NR_AGG_RETX_UNNECESSARY].mean, c.timeout_seconds * 1000.0, r->base_rtt_ms,
                        c.timeout_seconds * 1000.0 < r->base_rtt_ms ? ": the timer expires before an ACK can possibly return" : ": caused by lost/corrupted ACKs, queueing delay or go-back resends");
        }
    }

    /* Stop-and-Wait analytic bound. */
    for (unsigned si = 0; si < points; ++si) {
        const NrExperimentAggregate *a = find_aggregate(r, si, NR_ARQ_STOP_WAIT);
        if (a == NULL || !(r->base_rtt_ms > 0.0) || (points > 1U && si != 0U)) continue;
        NrArqConfig c = r->spec.config;
        apply_config_value(&c, r->spec.sweep, r->spec.sweep_count > 0U ? r->spec.sweep_values[si] : 0.0);
        const double bound = (double)c.payload_bytes * 8.0 / (r->base_rtt_ms / 1000.0);
        point_label(r, si, where, sizeof(where));
        format_value(v1, sizeof(v1), NR_AGG_GOODPUT, a->stats[NR_AGG_GOODPUT].mean);
        format_value(v2, sizeof(v2), NR_AGG_GOODPUT, bound);
        add_finding(r, "%s, Stop-and-Wait goodput %s against its one-packet-per-RTT ceiling of %s (payload / no-load RTT %.1f ms): only one frame is ever outstanding.",
                    where, v1, v2, r->base_rtt_ms);
    }

    /* Faults per point. */
    for (unsigned si = 0; si < points; ++si) {
        double inj = 0.0, rec = 0.0, loss = 0.0, cor = 0.0, ackl = 0.0, qd = 0.0; unsigned n = 0U;
        for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
            const NrExperimentAggregate *a = find_aggregate(r, si, (NrArqProtocol)p);
            if (a == NULL) continue;
            inj += a->stats[NR_AGG_FAULTS_INJECTED].mean; rec += a->stats[NR_AGG_FAULTS_RECOVERED].mean;
            loss += a->stats[NR_AGG_DATA_LOSSES].mean; cor += a->stats[NR_AGG_CORRUPTIONS_DETECTED].mean;
            ackl += a->stats[NR_AGG_ACK_LOSSES].mean; qd += a->stats[NR_AGG_QUEUE_DROPS].mean; ++n;
        }
        if (n == 0U || inj <= 0.0) continue;
        point_label(r, si, where, sizeof(where));
        add_finding(r, "%s, observed faults per run (averaged over protocols): %.1f data losses, %.1f detected data corruptions, %.1f ACK losses, %.1f queue drops; %.1f%% of all injected faults were recovered.",
                    where, loss / n, cor / n, ackl / n, qd / n, 100.0 * rec / inj);
    }

    /* Sweep trends. */
    if (points > 1U) {
        for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
            const NrExperimentAggregate *first = find_aggregate(r, 0U, (NrArqProtocol)p), *last = find_aggregate(r, points - 1U, (NrArqProtocol)p);
            if (first == NULL || last == NULL) continue;
            const double g0 = first->stats[NR_AGG_GOODPUT].mean, g1 = last->stats[NR_AGG_GOODPUT].mean;
            const double t0 = first->stats[NR_AGG_COMPLETION_TIME].mean, t1 = last->stats[NR_AGG_COMPLETION_TIME].mean;
            const double r0 = first->stats[NR_AGG_RETRANSMISSIONS].mean, r1 = last->stats[NR_AGG_RETRANSMISSIONS].mean;
            format_value(v1, sizeof(v1), NR_AGG_GOODPUT, g0); format_value(v2, sizeof(v2), NR_AGG_GOODPUT, g1);
            char from[48], to[48];
            if (r->spec.sweep == NR_SWEEP_CONGESTION_MODE) {
                (void)snprintf(from, sizeof(from), "%s", nr_congestion_mode_name((NrCongestionMode)(int)r->spec.sweep_values[0]));
                (void)snprintf(to, sizeof(to), "%s", nr_congestion_mode_name((NrCongestionMode)(int)r->spec.sweep_values[points - 1U]));
            } else {
                (void)snprintf(from, sizeof(from), "%g", r->spec.sweep_values[0]);
                (void)snprintf(to, sizeof(to), "%g", r->spec.sweep_values[points - 1U]);
            }
            add_finding(r, "Trend for %s as %s goes from %s to %s: goodput %s -> %s (%+.1f%%), completion time %.3f s -> %.3f s, retransmissions %.1f -> %.1f.",
                        nr_arq_protocol_name((NrArqProtocol)p), nr_sweep_name(r->spec.sweep), from, to,
                        v1, v2, g0 > 0.0 ? 100.0 * (g1 - g0) / g0 : 0.0, t0, t1, r0, r1);
        }
    }

    /* Queues and congestion. */
    for (unsigned si = 0; si < points; ++si) {
        for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
            const NrExperimentAggregate *a = find_aggregate(r, si, (NrArqProtocol)p);
            if (a == NULL || !(a->stats[NR_AGG_QUEUE_DROPS].mean > 0.0 || a->stats[NR_AGG_CONGESTION_DECREASES].mean > 0.0)) continue;
            point_label(r, si, where, sizeof(where));
            add_finding(r, "%s, %s: %.1f queue drops, peak queue %.0f frames, mean bottleneck occupancy %.2f frames, %.1f congestion-control decreases.",
                        where, nr_arq_protocol_name((NrArqProtocol)p), a->stats[NR_AGG_QUEUE_DROPS].mean, a->stats[NR_AGG_QUEUE_MAX].max,
                        a->stats[NR_AGG_QUEUE_AVG].mean, a->stats[NR_AGG_CONGESTION_DECREASES].mean);
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------------------------- */
void nr_experiment_free(NrExperimentResult *r) {
    if (r == NULL) return;
    for (size_t i = 0; i < r->run_count; ++i) { free(r->runs[i].trace); free(r->runs[i].faults); }
    free(r->runs); free(r->aggregates);
    r->runs = NULL; r->aggregates = NULL; r->run_count = r->aggregate_count = 0U;
}

static bool fail(NrExperimentResult *r, const char *message) { (void)snprintf(r->error, sizeof(r->error), "%s", message); return false; }

bool nr_experiment_run(const NrNetwork *network, const NrExperimentSpec *spec, NrExperimentResult *r) {
    if (r == NULL) return false;
    memset(r, 0, sizeof(*r));
    if (network == NULL || spec == NULL) return fail(r, "missing network or specification");
    r->spec = *spec;
    NrExperimentSpec *sp = &r->spec;
    if ((sp->protocol_mask & 7U) == 0U) return fail(r, "select at least one protocol");
    if (sp->trials == 0U || sp->trials > NR_EXP_MAX_TRIALS) return fail(r, "trials must be between 1 and 200");
    if (sp->sweep_count > NR_EXP_MAX_SWEEP) return fail(r, "at most 12 sweep values");
    if (sp->sweep != NR_SWEEP_NONE && sp->sweep_count == 0U) return fail(r, "sweep parameter given without values");
    if (sp->sweep == NR_SWEEP_NONE) sp->sweep_count = 0U;
    const unsigned points = sp->sweep_count == 0U ? 1U : (unsigned)sp->sweep_count;
    unsigned protocols = 0U;
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) if (sp->protocol_mask & (1U << p)) ++protocols;
    const size_t total = (size_t)points * protocols * sp->trials;
    if (total > MAX_TOTAL_RUNS) return fail(r, "too many runs requested (limit 4000 = values x protocols x trials)");

    NrNetwork *base = malloc(sizeof(*base)), *copy = malloc(sizeof(*copy));
    NrArqSimulation *sim = malloc(sizeof(*sim));
    r->runs = calloc(total, sizeof(*r->runs));
    r->aggregates = calloc((size_t)points * protocols, sizeof(*r->aggregates));
    double *values = malloc(sp->trials * sizeof(*values));
    if (base == NULL || copy == NULL || sim == NULL || r->runs == NULL || r->aggregates == NULL || values == NULL) {
        free(base); free(copy); free(sim); free(values); nr_experiment_free(r); return fail(r, "out of memory");
    }
    *base = *network;
    for (size_t j = 0; j < base->link_count; ++j) {
        if (sp->link_delay_ms >= 0.0) base->links[j].delay_ms = sp->link_delay_ms;
        if (sp->link_bandwidth_mbps >= 0.0) base->links[j].bandwidth_mbps = sp->link_bandwidth_mbps;
        if (sp->link_jitter_ms >= 0.0) base->links[j].jitter_ms = sp->link_jitter_ms;
    }
    if (sp->queue_capacity >= 0) for (size_t i = 0; i < base->node_count; ++i) base->nodes[i].queue_capacity = (unsigned)sp->queue_capacity;
    if (sp->source < 0 || sp->destination < 0) (void)nr_default_endpoints(base, &sp->source, &sp->destination);
    r->route = nr_find_route(base, sp->source, sp->destination);
    const NrRoute reverse = nr_find_route(base, sp->destination, sp->source);
    if (!r->route.reachable || !reverse.reachable) {
        free(base); free(copy); free(sim); free(values); nr_experiment_free(r);
        return fail(r, "destination is unreachable from the source in the selected topology");
    }
    r->base_rtt_ms = nr_route_base_rtt_ms(base, &r->route, &reverse, sp->config.payload_bytes);

    bool ok = true;
    for (unsigned si = 0; si < points && ok; ++si) {
        const double value = sp->sweep_count > 0U ? sp->sweep_values[si] : 0.0;
        for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT && ok; ++p) {
            if ((sp->protocol_mask & (1U << p)) == 0U) continue;
            for (unsigned t = 0; t < sp->trials; ++t) {
                *copy = *base;
                apply_network_value(copy, sp->sweep, value);
                NrArqConfig config = sp->config;
                config.protocol = (NrArqProtocol)p;
                config.seed = sp->seed_base + t * sp->seed_stride;
                apply_config_value(&config, sp->sweep, value);
                if (!nr_arq_start_scenario(sim, copy, &config, &sp->scenario, sp->source, sp->destination)) {
                    char message[160];
                    (void)snprintf(message, sizeof(message), "invalid configuration for %s at %s = %g", nr_arq_protocol_name((NrArqProtocol)p), nr_sweep_name(sp->sweep), value);
                    (void)fail(r, message); ok = false; break;
                }
                const bool capture = sp->capture_trace && t == 0U;
                sim->recording = capture;
                nr_arq_run(sim);
                NrExperimentRun *run = &r->runs[r->run_count++];
                run->sweep_index = si; run->sweep_value = value; run->trial = t; run->seed = config.seed; run->protocol = (NrArqProtocol)p;
                nr_arq_compute_metrics(sim, &run->metrics);
                collect_evidence(sim, run);
                if (capture) {
                    run->trace = sim->trace; run->trace_count = sim->trace_count; sim->trace = NULL;
                    run->faults = sim->faults; run->fault_count = sim->fault_count; sim->faults = NULL;
                }
                nr_arq_free(sim);
            }
            if (!ok) break;
            NrExperimentAggregate *a = &r->aggregates[r->aggregate_count++];
            a->sweep_index = si; a->sweep_value = value; a->protocol = (NrArqProtocol)p;
            const size_t first = r->run_count - sp->trials;
            a->runs = sp->trials;
            for (size_t j = first; j < r->run_count; ++j) if (r->runs[j].metrics.completed) ++a->completed;
            for (int k = 0; k < NR_AGG_COUNT; ++k) {
                unsigned n = 0U;
                for (size_t j = first; j < r->run_count; ++j) { double v; if (metric_value(&r->runs[j].metrics, (NrAggregateMetric)k, &v)) values[n++] = v; }
                a->stats[k] = compute_stat(values, n);
            }
        }
    }
    free(base); free(copy); free(sim); free(values);
    if (!ok) { nr_experiment_free(r); return false; }
    generate_findings(r);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * CSV export
 * ------------------------------------------------------------------------------------------- */
typedef struct { char *data; size_t length, capacity; bool ok; } Csv;

static void csv_put(Csv *c, const char *format, ...) {
    if (!c->ok) return;
    for (;;) {
        va_list args; va_start(args, format);
        const int n = vsnprintf(c->data + c->length, c->capacity - c->length, format, args);
        va_end(args);
        if (n < 0) { c->ok = false; return; }
        if ((size_t)n < c->capacity - c->length) { c->length += (size_t)n; return; }
        char *grown = realloc(c->data, c->capacity * 2U + (size_t)n);
        if (grown == NULL) { c->ok = false; return; }
        c->data = grown; c->capacity = c->capacity * 2U + (size_t)n;
    }
}

static void csv_opt(Csv *c, bool present, double v) { if (present && isfinite(v)) csv_put(c, ",%.10g", v); else csv_put(c, ","); }

char *nr_experiment_csv(const NrExperimentResult *r) {
    if (r == NULL) return NULL;
    Csv c = {malloc(8192), 0U, 8192U, true};
    if (c.data == NULL) return NULL;
    c.data[0] = '\0';
    csv_put(&c, "sweep_parameter,sweep_value,protocol,trial,seed,completed,stop_reason,packets_requested,packets_delivered,"
                "delivery_ratio,goodput_bps,throughput_bps,data_throughput_bps,latency_avg_ms,latency_min_ms,latency_max_ms,"
                "latency_stddev_ms,completion_time_s,data_transmissions,first_transmissions,retransmissions,retx_timeout,retx_nak,"
                "retx_goback,retx_unnecessary,retransmission_rate,efficiency,frame_efficiency,timeouts,acks_sent,naks_sent,"
                "dup_acks_sent,duplicates_received,out_of_order_received,data_losses,data_corruptions_injected,"
                "data_corruptions_detected,ack_losses,ack_corruptions_injected,ack_corruptions_detected,queue_overflow_drops,"
                "link_outage_drops,no_route_drops,faults_total,faults_recovered,faults_unrecovered,queue_avg_occupancy,"
                "queue_max_avg_occupancy,queue_max_occupancy,congestion_decreases,congestion_increases,rtt_avg_ms,retransmitted_sequences\n");
    for (size_t i = 0; i < r->run_count; ++i) {
        const NrExperimentRun *run = &r->runs[i];
        const NrArqMetrics *m = &run->metrics;
        csv_put(&c, "%s,", nr_sweep_name(r->spec.sweep));
        if (r->spec.sweep != NR_SWEEP_NONE) csv_put(&c, "%.10g", run->sweep_value);
        csv_put(&c, ",%s,%u,%u,%d,%s,%llu,%llu", nr_arq_protocol_name(run->protocol), run->trial, run->seed, m->completed ? 1 : 0,
                nr_arq_stop_reason_name(m->stop_reason), (unsigned long long)m->packets_requested, (unsigned long long)m->packets_delivered);
        csv_opt(&c, m->has_delivery_ratio, m->delivery_ratio);
        csv_opt(&c, m->has_goodput, m->goodput_bps); csv_opt(&c, m->has_goodput, m->throughput_bps); csv_opt(&c, m->has_goodput, m->data_throughput_bps);
        csv_opt(&c, m->has_latency, m->latency_avg_ms); csv_opt(&c, m->has_latency, m->latency_min_ms);
        csv_opt(&c, m->has_latency, m->latency_max_ms); csv_opt(&c, m->has_latency, m->latency_stddev_ms);
        csv_put(&c, ",%.10g,%llu,%llu,%llu,%llu,%llu,%llu,%llu", m->elapsed_seconds, (unsigned long long)m->data_transmissions,
                (unsigned long long)m->first_transmissions, (unsigned long long)m->retransmissions, (unsigned long long)m->retx_timeout,
                (unsigned long long)m->retx_nak, (unsigned long long)m->retx_goback, (unsigned long long)m->retx_unnecessary);
        csv_opt(&c, m->has_retx_rate, m->retransmission_rate); csv_opt(&c, m->has_efficiency, m->efficiency);
        csv_opt(&c, m->data_transmissions > 0U, m->frame_efficiency);
        csv_put(&c, ",%llu,%llu,%llu,%llu,%llu,%llu", (unsigned long long)m->timeouts, (unsigned long long)m->acks_sent,
                (unsigned long long)m->naks_sent, (unsigned long long)m->dup_acks_sent, (unsigned long long)m->duplicates_received,
                (unsigned long long)m->out_of_order_received);
        csv_put(&c, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu", (unsigned long long)m->faults[NR_FAULT_DATA_LOSS].injected,
                (unsigned long long)m->faults[NR_FAULT_DATA_CORRUPTION].injected, (unsigned long long)m->faults[NR_FAULT_DATA_CORRUPTION].detected,
                (unsigned long long)m->faults[NR_FAULT_ACK_LOSS].injected, (unsigned long long)m->faults[NR_FAULT_ACK_CORRUPTION].injected,
                (unsigned long long)m->faults[NR_FAULT_ACK_CORRUPTION].detected, (unsigned long long)m->faults[NR_FAULT_QUEUE_OVERFLOW].injected,
                (unsigned long long)m->faults[NR_FAULT_LINK_OUTAGE].injected, (unsigned long long)m->faults[NR_FAULT_NO_ROUTE].injected,
                (unsigned long long)m->faults_total, (unsigned long long)m->faults_recovered, (unsigned long long)m->faults_unrecovered);
        csv_put(&c, ",%.10g,%.10g,%u,%llu,%llu", m->queue_avg_occupancy, m->queue_max_avg_occupancy, m->queue_max_occupancy,
                (unsigned long long)m->congestion_decreases, (unsigned long long)m->congestion_increases);
        csv_opt(&c, m->has_rtt, m->rtt_avg_ms);
        csv_put(&c, ",\"");
        for (size_t q = 0; q < run->retransmitted_count; ++q) csv_put(&c, "%s%u", q == 0U ? "" : " ", run->retransmitted[q].sequence);
        csv_put(&c, "\"\n");
    }
    if (!c.ok) { free(c.data); return NULL; }
    return c.data;
}
