/* Engine tests: ARQ protocols, fault injection, metrics, experiments, persistence. Assertions stay on in Release. */
#include "arq.h"
#include "experiment.h"
#include "netrescue.h"
#include "topology.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); abort(); } } while (0)

static NrArqConfig base_config(NrArqProtocol protocol, size_t packets, unsigned window) {
    NrArqConfig c; nr_arq_default_config(&c);
    c.protocol = protocol; c.packet_count = packets; c.window_size = window; c.payload_bytes = 1000U;
    c.packets_per_second = 1000.0; c.timeout_seconds = 0.15; c.seed = 7U;
    return c;
}

/* H1 - R2 - R3 - H4, 10 Mbit/s and 10 ms per link: no-load RTT = 3*(10+0.816) + 3*(10+0.016) = 62.5 ms. */
static void line_network(NrNetwork *n) {
    CHECK(nr_generate_topology(n, NR_TOPO_LINE, 4U, 1U));
    for (size_t j = 0; j < n->link_count; ++j) { n->links[j].delay_ms = 10.0; n->links[j].bandwidth_mbps = 10.0; }
}

static NrArqSimulation *run_transfer(const NrNetwork *n, const NrArqConfig *c, const NrFaultScenario *sc) {
    NrArqSimulation *s = malloc(sizeof(*s));
    int src, dst; CHECK(nr_default_endpoints(n, &src, &dst));
    CHECK(nr_arq_start_scenario(s, n, c, sc, src, dst));
    nr_arq_run(s);
    return s;
}

static void destroy(NrArqSimulation *s) { nr_arq_free(s); free(s); }

static size_t count_trace(const NrArqSimulation *s, NrArqTraceType type, int seq) {
    size_t count = 0U;
    for (size_t i = 0; i < s->trace_count; ++i) if (s->trace[i].type == type && (seq < 0 || s->trace[i].sequence == seq)) ++count;
    return count;
}

static NrFaultRule rule(NrFaultRuleType type, unsigned seq, unsigned attempt) {
    NrFaultRule r; memset(&r, 0, sizeof(r)); r.type = type; r.sequence = seq; r.attempt = attempt; r.link_id = -1; r.node_id = -1; return r;
}

/* Every counter the metrics report must agree with the recorded event trace. */
static void check_metrics_match_trace(const NrArqSimulation *s) {
    NrArqMetrics m; nr_arq_compute_metrics(s, &m);
    CHECK(s->trace_dropped == 0U);
    CHECK(count_trace(s, NR_TR_DATA_SEND, -1) == s->stats.generated);
    CHECK(count_trace(s, NR_TR_DATA_RETX, -1) == m.retransmissions);
    CHECK(m.data_transmissions == m.first_transmissions + m.retransmissions);
    CHECK(count_trace(s, NR_TR_DATA_DELIVERED, -1) == m.packets_delivered);
    CHECK(count_trace(s, NR_TR_ACK_SEND, -1) == m.acks_sent);
    CHECK(count_trace(s, NR_TR_NAK_SEND, -1) == m.naks_sent);
    CHECK(count_trace(s, NR_TR_TIMEOUT, -1) == m.timeouts);
    CHECK(count_trace(s, NR_TR_DATA_LOST, -1) == s->stats.packet_losses);
    CHECK(count_trace(s, NR_TR_ACK_LOST, -1) == s->stats.ack_losses);
    CHECK(count_trace(s, NR_TR_DATA_CORRUPT_DETECTED, -1) == s->stats.corruptions);
    CHECK(count_trace(s, NR_TR_ACK_CORRUPT_DETECTED, -1) == s->stats.ack_corruptions);
    CHECK(count_trace(s, NR_TR_DATA_DUPLICATE, -1) == m.duplicates_received);
    CHECK(m.retx_timeout + m.retx_nak + m.retx_goback == m.retransmissions);
    /* Unique delivery: never more than requested, and retransmissions are never new packets. */
    CHECK(m.packets_delivered <= m.packets_requested);
    CHECK(m.first_transmissions <= m.packets_requested);
    for (size_t seq = 0; seq < s->config.packet_count; ++seq) CHECK(count_trace(s, NR_TR_DATA_DELIVERED, (int)seq) <= 1U);
    /* Formulas. */
    CHECK(m.data_bits == m.data_transmissions * (uint64_t)(s->config.payload_bytes + NR_ARQ_HEADER_BYTES) * 8U);
    CHECK(m.control_bits == (m.acks_sent + m.naks_sent) * (uint64_t)NR_ARQ_ACK_BYTES * 8U);
    if (m.elapsed_seconds > 0.0) {
        CHECK(fabs(m.goodput_bps - (double)m.packets_delivered * s->config.payload_bytes * 8.0 / m.elapsed_seconds) < 1e-6);
        CHECK(fabs(m.throughput_bps - (double)(m.data_bits + m.control_bits) / m.elapsed_seconds) < 1e-6);
    }
    CHECK(fabs(m.delivery_ratio - (double)m.packets_delivered / (double)m.packets_requested) < 1e-12);
    if (m.first_transmissions > 0U) CHECK(fabs(m.retransmission_rate - (double)m.retransmissions / (double)m.first_transmissions) < 1e-12);
    CHECK(fabs(m.efficiency - (double)m.packets_delivered * s->config.payload_bytes * 8.0 / (double)(m.data_bits + m.control_bits)) < 1e-12);
    double latency = 0.0; size_t delivered = 0U;
    for (size_t i = 0; i < s->config.packet_count; ++i) if (s->frames[i].delivered) { latency += (s->frames[i].delivered_at - s->frames[i].created_at) * 1000.0; ++delivered; }
    CHECK(delivered == m.packets_delivered);
    if (delivered > 0U) CHECK(fabs(m.latency_avg_ms - latency / (double)delivered) < 1e-6);
    uint64_t faults = 0U; for (int t = 0; t < NR_FAULT_TYPE_COUNT; ++t) faults += m.faults[t].injected;
    CHECK(faults == s->fault_count && faults == m.faults_total && m.faults_recovered + m.faults_unrecovered == faults);
}

static void test_clean_transfers(void) {
    NrNetwork n; line_network(&n);
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
        NrArqConfig c = base_config((NrArqProtocol)p, 30U, 8U);
        NrArqSimulation *s = run_transfer(&n, &c, NULL);
        NrArqMetrics m; nr_arq_compute_metrics(s, &m);
        CHECK(s->state == NR_ARQ_COMPLETE && m.completed && m.stop_reason == NR_STOP_COMPLETE);
        CHECK(m.packets_delivered == 30U && m.retransmissions == 0U && m.faults_total == 0U && m.retx_unnecessary == 0U);
        CHECK(m.acks_sent == 30U && s->stats.acknowledgements == 30U && m.duplicates_received == 0U);
        CHECK(fabs(m.delivery_ratio - 1.0) < 1e-12);
        /* Uncontended latency equals the one-way serialisation + propagation delay: 3 * (10 ms + 0.816 ms). */
        CHECK(fabs(m.latency_min_ms - 32.448) < 1e-6);
        check_metrics_match_trace(s);
        destroy(s);
    }
    /* Stop-and-Wait goodput hits the analytic bound payload / RTT exactly on an idle path. */
    NrArqConfig c = base_config(NR_ARQ_STOP_WAIT, 20U, 8U);
    NrArqSimulation *s = run_transfer(&n, &c, NULL);
    const NrRoute f = nr_find_route(&n, s->source, s->destination), r = nr_find_route(&n, s->destination, s->source);
    const double rtt = nr_route_base_rtt_ms(&n, &f, &r, c.payload_bytes);
    CHECK(fabs(rtt - 62.496) < 1e-6);
    CHECK(fabs(s->time_seconds - 20.0 * rtt / 1000.0) < 1e-9);
    destroy(s);
}

static void test_window_limits(void) {
    NrNetwork n; line_network(&n);
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
        NrArqConfig c = base_config((NrArqProtocol)p, 40U, 5U); c.loss_probability = 0.2; c.seed = 11U;
        NrArqSimulation *s = malloc(sizeof(*s)); int a, b; CHECK(nr_default_endpoints(&n, &a, &b));
        CHECK(nr_arq_start(s, &n, &c, a, b));
        const size_t window = p == NR_ARQ_STOP_WAIT ? 1U : 5U;
        CHECK(s->config.window_size == window);
        size_t max_outstanding = 0U;
        for (unsigned i = 0; i < 200000U && s->state == NR_ARQ_RUNNING; ++i) {
            nr_arq_step(s, 0.001);
            CHECK(s->next_sequence - s->sender_base <= window);
            if (s->next_sequence - s->sender_base > max_outstanding) max_outstanding = s->next_sequence - s->sender_base;
            if (p == NR_ARQ_SELECTIVE_REPEAT) for (size_t q = s->receiver_base + window; q < c.packet_count; ++q) CHECK(!s->receiver_buffer[q]);
        }
        CHECK(s->state == NR_ARQ_COMPLETE && max_outstanding == window);
        check_metrics_match_trace(s);
        destroy(s);
    }
}

/* The critical comparison: lose seq 4 once while 5..11 are outstanding. */
static void test_gbn_vs_sr_middle_loss(void) {
    NrNetwork n; line_network(&n);
    NrFaultScenario sc; memset(&sc, 0, sizeof(sc));
    NrFaultRule r = rule(NR_RULE_DROP_DATA, 4U, 1U); CHECK(nr_scenario_add(&sc, &r));
    NrArqSimulation *results[2]; const NrArqProtocol protocols[2] = {NR_ARQ_GO_BACK_N, NR_ARQ_SELECTIVE_REPEAT};
    for (int k = 0; k < 2; ++k) {
        NrArqConfig c = base_config(protocols[k], 24U, 8U);
        NrArqSimulation *s = results[k] = run_transfer(&n, &c, &sc);
        CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 24U);
        /* The fault happened exactly once, to seq 4's first copy, on the middle hop (R2 -> R3, link 2). */
        CHECK(s->fault_count == 1U && s->faults[0].type == NR_FAULT_DATA_LOSS && s->faults[0].source == NR_SRC_SCENARIO);
        CHECK(s->faults[0].sequence == 4U && s->faults[0].attempt == 1U && s->faults[0].link_id == 2);
        CHECK(s->faults[0].detected && s->faults[0].recovered && s->faults[0].response == NR_RESP_RETX_TIMEOUT);
        CHECK(count_trace(s, NR_TR_DATA_LOST, 4) == 1U && count_trace(s, NR_TR_DATA_LOST, -1) == 1U);
        check_metrics_match_trace(s);
    }
    NrArqSimulation *gbn = results[0], *sr = results[1];
    /* Go-Back-N resends 4 and every later outstanding frame (5..11), which its receiver discarded. */
    for (int seq = 0; seq < 24; ++seq) CHECK(count_trace(gbn, NR_TR_DATA_RETX, seq) == (seq >= 4 && seq <= 11 ? 1U : 0U));
    CHECK(gbn->stats.retx_timeout == 1U && gbn->stats.retx_goback == 7U);
    for (int seq = 5; seq <= 11; ++seq) CHECK(count_trace(gbn, NR_TR_DATA_DISCARDED_OUT_OF_ORDER, seq) == 1U);
    CHECK(count_trace(gbn, NR_TR_DATA_BUFFERED, -1) == 0U);
    /* Selective Repeat buffers 5..11 and resends only seq 4. */
    for (int seq = 0; seq < 24; ++seq) CHECK(count_trace(sr, NR_TR_DATA_RETX, seq) == (seq == 4 ? 1U : 0U));
    for (int seq = 5; seq <= 11; ++seq) CHECK(count_trace(sr, NR_TR_DATA_BUFFERED, seq) == 1U);
    CHECK(sr->stats.out_of_order == 7U && count_trace(sr, NR_TR_DATA_DISCARDED_OUT_OF_ORDER, -1) == 0U);
    /* In-order delivery to the application: SR delivers 5..11 only after 4 arrives. */
    for (int seq = 5; seq <= 11; ++seq) CHECK(sr->frames[seq].delivered_at >= sr->frames[4].delivered_at && sr->frames[seq].received_at < sr->frames[4].received_at);
    destroy(gbn); destroy(sr);
}

static void test_corruption_detection_and_recovery(void) {
    NrNetwork n; line_network(&n);
    NrFaultScenario sc; memset(&sc, 0, sizeof(sc));
    NrFaultRule r = rule(NR_RULE_CORRUPT_DATA, 5U, 1U); CHECK(nr_scenario_add(&sc, &r));
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
        for (int nak = 0; nak <= 1; ++nak) {
            NrArqConfig c = base_config((NrArqProtocol)p, 12U, 4U); c.use_nak = nak != 0;
            NrArqSimulation *s = run_transfer(&n, &c, &sc);
            CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 12U);
            CHECK(s->fault_count == 1U && s->faults[0].type == NR_FAULT_DATA_CORRUPTION && s->faults[0].sequence == 5U);
            /* Detection is a real checksum comparison, not a flag: the receiver rejected the frame. */
            CHECK(s->faults[0].detected && s->faults[0].recovered && s->stats.corruptions == 1U && s->stats.undetected_corruptions == 0U);
            CHECK(count_trace(s, NR_TR_CORRUPTION_INJECTED, 5) == 1U && count_trace(s, NR_TR_DATA_CORRUPT_DETECTED, 5) == 1U);
            CHECK(s->frames[5].accepted_attempt == 2U);
            if (nak) { CHECK(s->stats.naks_sent == 1U && s->stats.negative_acks == 1U && s->faults[0].response == NR_RESP_RETX_NAK); }
            else { CHECK(s->stats.naks_sent == 0U && s->faults[0].response == NR_RESP_RETX_TIMEOUT && s->stats.timeouts >= 1U); }
            check_metrics_match_trace(s);
            destroy(s);
        }
    }
    /* Multi-bit burst corruption is also caught by the checksum. */
    NrArqConfig c = base_config(NR_ARQ_SELECTIVE_REPEAT, 60U, 8U); c.corruption_probability = 0.3; c.corruption_bits = 17U;
    NrArqSimulation *s = run_transfer(&n, &c, NULL);
    CHECK(s->state == NR_ARQ_COMPLETE && s->stats.corruptions > 5U && s->stats.undetected_corruptions == 0U);
    check_metrics_match_trace(s);
    destroy(s);
}

static void test_ack_loss_and_duplicates(void) {
    NrNetwork n; line_network(&n);
    NrFaultScenario sc; memset(&sc, 0, sizeof(sc));
    NrFaultRule r = rule(NR_RULE_DROP_ACK, 3U, 1U); CHECK(nr_scenario_add(&sc, &r));
    /* SW and SR: data arrived but its ACK was lost -> timeout -> duplicate data -> re-ACK, never delivered twice. */
    for (int k = 0; k < 2; ++k) {
        NrArqConfig c = base_config(k == 0 ? NR_ARQ_STOP_WAIT : NR_ARQ_SELECTIVE_REPEAT, 10U, 4U);
        NrArqSimulation *s = run_transfer(&n, &c, &sc);
        CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 10U);
        CHECK(s->fault_count == 1U && s->faults[0].type == NR_FAULT_ACK_LOSS && s->faults[0].frame_kind == NR_ARQ_ACK_EVENT && s->faults[0].sequence == 3U);
        CHECK(s->faults[0].response == NR_RESP_RETX_TIMEOUT && s->faults[0].recovered);
        CHECK(s->frames[3].received_at < s->faults[0].time); /* the data had already arrived */
        CHECK(s->stats.retransmissions == 1U && count_trace(s, NR_TR_DATA_RETX, 3) == 1U);
        CHECK(s->stats.duplicates == 1U && count_trace(s, NR_TR_DATA_DUPLICATE, 3) == 1U && count_trace(s, NR_TR_DATA_DELIVERED, 3) == 1U);
        NrArqMetrics m; nr_arq_compute_metrics(s, &m);
        CHECK(m.retx_unnecessary == 1U);
        check_metrics_match_trace(s);
        destroy(s);
    }
    /* GBN: the next cumulative ACK covers the lost one, so nothing is retransmitted. */
    NrArqConfig c = base_config(NR_ARQ_GO_BACK_N, 10U, 4U);
    NrArqSimulation *s = run_transfer(&n, &c, &sc);
    CHECK(s->state == NR_ARQ_COMPLETE && s->stats.retransmissions == 0U && s->faults[0].response == NR_RESP_COVERED_BY_ACK && s->faults[0].recovered);
    check_metrics_match_trace(s);
    destroy(s);
    /* ACK corruption is detected by the sender's checksum and discarded. */
    memset(&sc, 0, sizeof(sc)); r = rule(NR_RULE_CORRUPT_ACK, 2U, 1U); CHECK(nr_scenario_add(&sc, &r));
    c = base_config(NR_ARQ_SELECTIVE_REPEAT, 6U, 4U);
    s = run_transfer(&n, &c, &sc);
    CHECK(s->state == NR_ARQ_COMPLETE && s->stats.ack_corruptions == 1U && s->faults[0].type == NR_FAULT_ACK_CORRUPTION && s->faults[0].detected);
    CHECK(count_trace(s, NR_TR_ACK_CORRUPT_DETECTED, 2) == 1U && s->stats.retransmissions == 1U && s->stats.duplicates == 1U);
    check_metrics_match_trace(s);
    destroy(s);
}

static void test_retry_limit_and_timeouts(void) {
    NrNetwork n; line_network(&n);
    NrArqConfig c = base_config(NR_ARQ_STOP_WAIT, 3U, 1U); c.loss_probability = 1.0; c.max_retries = 2U;
    NrArqSimulation *s = run_transfer(&n, &c, NULL);
    NrArqMetrics m; nr_arq_compute_metrics(s, &m);
    CHECK(s->state == NR_ARQ_FAILED && s->stop_reason == NR_STOP_RETRY_LIMIT && !m.completed);
    CHECK(m.data_transmissions == 3U && m.retransmissions == 2U && m.timeouts == 3U && m.packets_delivered == 0U && m.packets_undelivered == 3U);
    CHECK(!m.has_latency && m.faults_unrecovered == 3U);
    check_metrics_match_trace(s);
    destroy(s);
    /* Zero retries: the first loss aborts the transfer. */
    c.max_retries = 0U; s = run_transfer(&n, &c, NULL);
    CHECK(s->state == NR_ARQ_FAILED && s->stats.data_transmissions == 1U);
    destroy(s);
    /* Time limit is a stopping criterion that is reported explicitly. */
    c = base_config(NR_ARQ_STOP_WAIT, 100U, 1U); c.max_sim_time = 0.5;
    s = run_transfer(&n, &c, NULL); nr_arq_compute_metrics(s, &m);
    CHECK(s->state == NR_ARQ_FAILED && m.stop_reason == NR_STOP_TIME_LIMIT && fabs(m.elapsed_seconds - 0.5) < 1e-9 && m.packets_delivered > 0U && m.packets_delivered < 100U);
    destroy(s);
    /* A timeout shorter than the RTT: every retransmission is unnecessary. */
    c = base_config(NR_ARQ_STOP_WAIT, 10U, 1U); c.timeout_seconds = 0.03;
    s = run_transfer(&n, &c, NULL); nr_arq_compute_metrics(s, &m);
    CHECK(s->state == NR_ARQ_COMPLETE && m.retransmissions > 0U && m.retx_unnecessary == m.retransmissions && m.faults_total == 0U);
    check_metrics_match_trace(s);
    destroy(s);
}

static void test_burst_and_link_outage(void) {
    NrNetwork n; line_network(&n);
    NrFaultScenario sc; memset(&sc, 0, sizeof(sc));
    NrFaultRule r = rule(NR_RULE_BURST_LOSS, 2U, 1U); r.count = 4U; CHECK(nr_scenario_add(&sc, &r));
    NrArqConfig c = base_config(NR_ARQ_SELECTIVE_REPEAT, 10U, 8U);
    NrArqSimulation *s = run_transfer(&n, &c, &sc);
    CHECK(s->state == NR_ARQ_COMPLETE && s->fault_count == 4U);
    for (int seq = 0; seq < 10; ++seq) CHECK(count_trace(s, NR_TR_DATA_LOST, seq) == (seq >= 2 && seq <= 5 ? 1U : 0U));
    destroy(s);
    /* Temporary outage of a ring link on the route: traffic reroutes, in-flight frames on the link are lost. */
    CHECK(nr_generate_topology(&n, NR_TOPO_RING, 6U, 1U));
    int src, dst; CHECK(nr_default_endpoints(&n, &src, &dst));
    const NrRoute route = nr_find_route(&n, src, dst);
    const int on_route = nr_find_link_between(&n, route.nodes[1], route.nodes[2], NULL);
    memset(&sc, 0, sizeof(sc));
    NrFaultRule outage = rule(NR_RULE_LINK_OUTAGE, 0U, 0U); outage.link_id = n.links[on_route].id; outage.start = 0.05; outage.end = 0.4;
    CHECK(nr_scenario_add(&sc, &outage));
    c = base_config(NR_ARQ_SELECTIVE_REPEAT, 60U, 8U); c.packets_per_second = 200.0;
    s = run_transfer(&n, &c, &sc);
    CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 60U);
    CHECK(count_trace(s, NR_TR_LINK_DOWN, -1) == 1U && count_trace(s, NR_TR_LINK_UP, -1) == 1U && count_trace(s, NR_TR_REROUTE, -1) >= 2U);
    NrArqMetrics m; nr_arq_compute_metrics(s, &m);
    CHECK(m.faults[NR_FAULT_LINK_OUTAGE].injected > 0U && m.faults_unrecovered == 0U);
    check_metrics_match_trace(s);
    destroy(s);
    /* An invalid outage window is rejected. */
    NrFaultRule bad = outage; bad.end = bad.start; CHECK(!nr_scenario_add(&sc, &bad));
}

static void test_queue_overflow_and_congestion(void) {
    NrNetwork n; NrExperimentSpec spec;
    CHECK(nr_preset_build((size_t)nr_preset_find("congestion"), &n, &spec));
    uint64_t drops[NR_CC_MODE_COUNT];
    for (int mode = 0; mode < NR_CC_MODE_COUNT; ++mode) {
        NrArqConfig c = spec.config; c.protocol = NR_ARQ_GO_BACK_N; c.congestion_mode = (NrCongestionMode)mode;
        NrArqSimulation *s = run_transfer(&n, &c, NULL);
        NrArqMetrics m; nr_arq_compute_metrics(s, &m);
        CHECK(s->state == NR_ARQ_COMPLETE);
        CHECK(m.queue_max_occupancy <= 5U);
        drops[mode] = m.queue_drops;
        CHECK(m.faults[NR_FAULT_QUEUE_OVERFLOW].injected == m.queue_drops);
        if (mode != NR_CC_NONE) CHECK(m.congestion_decreases > 0U);
        check_metrics_match_trace(s);
        destroy(s);
    }
    CHECK(drops[NR_CC_NONE] > 0U && drops[NR_CC_WINDOW_AIMD] < drops[NR_CC_NONE] && drops[NR_CC_RATE_AIMD] < drops[NR_CC_NONE]);
    /* A queue_limit rule forces overflow at a chosen router. */
    NrNetwork line; line_network(&line);
    for (size_t j = 0; j < line.link_count; ++j) line.links[j].bandwidth_mbps = j == 1U ? 0.5 : 100.0;
    NrFaultScenario sc; memset(&sc, 0, sizeof(sc));
    NrFaultRule limit = rule(NR_RULE_QUEUE_LIMIT, 0U, 0U); limit.node_id = line.nodes[1].id; limit.capacity = 1U;
    CHECK(nr_scenario_add(&sc, &limit));
    NrArqConfig c = base_config(NR_ARQ_SELECTIVE_REPEAT, 20U, 8U);
    NrArqSimulation *s = run_transfer(&line, &c, &sc);
    NrArqMetrics m; nr_arq_compute_metrics(s, &m);
    CHECK(s->state == NR_ARQ_COMPLETE && m.queue_drops > 0U && m.queue_max_occupancy == 1U);
    check_metrics_match_trace(s);
    destroy(s);
}

static void test_random_faults_all_protocols(void) {
    NrNetwork n; CHECK(nr_generate_topology(&n, NR_TOPO_RING, 8U, 1U));
    for (size_t j = 0; j < n.link_count; ++j) { n.links[j].jitter_ms = 3.0; n.links[j].delay_ms = 5.0; }
    for (int p = 0; p < NR_ARQ_PROTOCOL_COUNT; ++p) {
        NrArqConfig c = base_config((NrArqProtocol)p, 80U, 8U);
        c.loss_probability = 0.1; c.corruption_probability = 0.05; c.ack_loss_probability = 0.1; c.ack_corruption_probability = 0.05;
        NrArqSimulation *s = run_transfer(&n, &c, NULL);
        CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 80U && s->stats.generated == 80U);
        CHECK(s->stats.packet_losses > 0U && s->stats.corruptions > 0U && s->stats.ack_losses > 0U && s->stats.ack_corruptions > 0U);
        NrArqMetrics m; nr_arq_compute_metrics(s, &m);
        CHECK(m.faults_unrecovered == 0U);
        check_metrics_match_trace(s);
        destroy(s);
    }
}

static void test_reproducibility_and_isolation(void) {
    NrNetwork n; NrExperimentSpec spec;
    CHECK(nr_preset_build((size_t)nr_preset_find("loss_sweep"), &n, &spec));
    spec.trials = 3U;
    NrExperimentResult *a = malloc(sizeof(*a)), *b = malloc(sizeof(*b)), *c = malloc(sizeof(*c));
    CHECK(nr_experiment_run(&n, &spec, a));
    /* Run something different in between, then repeat: results must be bit-identical. */
    NrExperimentSpec other = spec; other.config.window_size = 3U; other.protocol_mask = 1U;
    CHECK(nr_experiment_run(&n, &other, c)); nr_experiment_free(c);
    CHECK(nr_experiment_run(&n, &spec, b));
    CHECK(a->run_count == 45U && b->run_count == a->run_count);
    for (size_t i = 0; i < a->run_count; ++i) {
        CHECK(a->runs[i].seed == b->runs[i].seed);
        CHECK(memcmp(&a->runs[i].metrics, &b->runs[i].metrics, sizeof(NrArqMetrics)) == 0);
    }
    /* Seed schedule: trial i uses base + i * stride, identical for every protocol. */
    for (size_t i = 0; i < a->run_count; ++i) CHECK(a->runs[i].seed == spec.seed_base + a->runs[i].trial * spec.seed_stride);
    /* Protocol subset does not change a protocol's own results (runs are isolated). */
    NrExperimentSpec only_sr = spec; only_sr.protocol_mask = 1U << NR_ARQ_SELECTIVE_REPEAT;
    CHECK(nr_experiment_run(&n, &only_sr, c));
    size_t matched = 0U;
    for (size_t i = 0; i < c->run_count; ++i)
        for (size_t j = 0; j < a->run_count; ++j)
            if (a->runs[j].protocol == NR_ARQ_SELECTIVE_REPEAT && a->runs[j].sweep_index == c->runs[i].sweep_index && a->runs[j].trial == c->runs[i].trial) {
                CHECK(memcmp(&a->runs[j].metrics, &c->runs[i].metrics, sizeof(NrArqMetrics)) == 0); ++matched;
            }
    CHECK(matched == c->run_count && matched == 15U);
    /* Different seeds produce different impairment patterns at non-zero loss. */
    bool differs = false;
    for (size_t i = 0; i + 1U < a->run_count; ++i)
        if (a->runs[i].sweep_index == 4U && a->runs[i + 1U].sweep_index == 4U && a->runs[i].protocol == a->runs[i + 1U].protocol &&
            a->runs[i].metrics.retransmissions != a->runs[i + 1U].metrics.retransmissions) differs = true;
    CHECK(differs);
    /* Aggregates: mean/stddev/CI computed over the trials. */
    const NrExperimentAggregate *agg = &a->aggregates[a->aggregate_count - 1U];
    double sum = 0.0, values[3]; unsigned k = 0U;
    for (size_t i = 0; i < a->run_count; ++i) if (a->runs[i].sweep_index == agg->sweep_index && a->runs[i].protocol == agg->protocol) { values[k] = (double)a->runs[i].metrics.retransmissions; sum += values[k++]; }
    CHECK(k == 3U);
    const double mean = sum / 3.0;
    double sq = 0.0; for (unsigned i = 0; i < 3U; ++i) sq += (values[i] - mean) * (values[i] - mean);
    CHECK(fabs(agg->stats[NR_AGG_RETRANSMISSIONS].mean - mean) < 1e-9);
    CHECK(fabs(agg->stats[NR_AGG_RETRANSMISSIONS].stddev - sqrt(sq / 2.0)) < 1e-9);
    CHECK(fabs(agg->stats[NR_AGG_RETRANSMISSIONS].ci95 - 4.303 * sqrt(sq / 2.0) / sqrt(3.0)) < 1e-9);
    CHECK(a->finding_count > 0U);
    nr_experiment_free(a); nr_experiment_free(b); nr_experiment_free(c); free(a); free(b); free(c);
}

static void test_all_presets_run(void) {
    for (size_t i = 0; i < nr_preset_count(); ++i) {
        NrNetwork *n = malloc(sizeof(*n)); NrExperimentSpec spec; NrExperimentResult *r = malloc(sizeof(*r));
        CHECK(nr_preset_build(i, n, &spec));
        CHECK(nr_experiment_run(n, &spec, r));
        CHECK(r->run_count > 0U && r->finding_count > 0U && r->base_rtt_ms > 0.0);
        for (size_t k = 0; k < r->run_count; ++k) CHECK(r->runs[k].metrics.completed && r->runs[k].metrics.packets_delivered == spec.config.packet_count);
        char *csv = nr_experiment_csv(r);
        CHECK(csv != NULL);
        size_t lines = 0U; for (const char *p = csv; *p != '\0'; ++p) if (*p == '\n') ++lines;
        CHECK(lines == r->run_count + 1U);
        free(csv);
        if (strcmp(nr_preset_info(i)->id, "gbn_vs_sr") == 0) {
            CHECK(r->run_count == 2U);
            const NrExperimentRun *g = &r->runs[0], *s = &r->runs[1];
            CHECK(g->protocol == NR_ARQ_GO_BACK_N && g->retransmitted_count == 8U && g->retransmitted[0].sequence == 4U && g->retransmitted[7].sequence == 11U);
            CHECK(s->protocol == NR_ARQ_SELECTIVE_REPEAT && s->retransmitted_count == 1U && s->retransmitted[0].sequence == 4U);
            CHECK(g->trace != NULL && s->trace != NULL);
        }
        nr_experiment_free(r); free(r); free(n);
    }
    NrExperimentSpec bad; nr_experiment_default_spec(&bad); bad.protocol_mask = 0U;
    NrNetwork n; CHECK(nr_load_demo(&n));
    NrExperimentResult *r = malloc(sizeof(*r));
    CHECK(!nr_experiment_run(&n, &bad, r) && strlen(r->error) > 0U);
    free(r);
}

static void test_step_event_and_controls(void) {
    NrNetwork n; line_network(&n);
    NrArqConfig c = base_config(NR_ARQ_GO_BACK_N, 5U, 4U);
    NrArqSimulation *s = malloc(sizeof(*s)); int a, b; CHECK(nr_default_endpoints(&n, &a, &b));
    CHECK(nr_arq_start(s, &n, &c, a, b));
    nr_arq_pause(s); CHECK(s->state == NR_ARQ_PAUSED);
    const size_t before = s->trace_count;
    CHECK(nr_arq_step_event(s));
    CHECK(s->state == NR_ARQ_PAUSED && s->trace_count > before && s->trace[s->trace_count - 1U].type == NR_TR_DATA_SEND);
    double t = s->time_seconds;
    for (int i = 0; i < 50; ++i) { CHECK(nr_arq_step_event(s)); CHECK(s->time_seconds >= t); t = s->time_seconds; }
    nr_arq_step(s, 1.0); CHECK(s->time_seconds == t); /* paused: real-time stepping does nothing */
    nr_arq_resume(s); nr_arq_run(s);
    CHECK(s->state == NR_ARQ_COMPLETE && s->stats.delivered == 5U);
    CHECK(!nr_arq_step_event(s));
    destroy(s);
    /* Invalid configurations are rejected. */
    NrArqSimulation t2; c = base_config(NR_ARQ_GO_BACK_N, 0U, 4U);
    CHECK(!nr_arq_start(&t2, &n, &c, a, b));
    c = base_config(NR_ARQ_GO_BACK_N, 5U, 4U); c.loss_probability = 1.5; CHECK(!nr_arq_start(&t2, &n, &c, a, b));
    c = base_config(NR_ARQ_GO_BACK_N, 5U, 4U); c.timeout_seconds = 0.0; CHECK(!nr_arq_start(&t2, &n, &c, a, b));
    c = base_config(NR_ARQ_GO_BACK_N, 5U, 4U); CHECK(!nr_arq_start(&t2, &n, &c, a, a));
}

static void test_topologies_validation_and_persistence(void) {
    NrNetwork n;
    for (int type = 0; type < NR_TOPO_COUNT; ++type) {
        for (size_t count = 2U; count <= 20U; count += 3U) {
            CHECK(nr_generate_topology(&n, (NrTopologyType)type, count, 5U));
            CHECK(n.node_count == count && n.link_count >= count - 1U);
            for (size_t i = 0; i < n.node_count; ++i) CHECK(n.nodes[i].x >= 0.0F && n.nodes[i].x <= 1.0F && n.nodes[i].y >= 0.0F && n.nodes[i].y <= 1.0F);
            int src, dst; CHECK(nr_default_endpoints(&n, &src, &dst) && src != dst);
            CHECK(nr_is_connected(&n, src, dst));
            NrIssue issues[NR_MAX_ISSUES];
            const size_t found = nr_validate_network(&n, src, dst, issues);
            for (size_t i = 0; i < found; ++i) CHECK(issues[i].severity != NR_ISSUE_ERROR);
        }
    }
    CHECK(nr_generate_topology(&n, NR_TOPO_STAR, 7U, 1U) && n.nodes[3].type == NR_SWITCH && n.nodes[0].type == NR_HOST);
    CHECK(nr_generate_topology(&n, NR_TOPO_TREE, 7U, 1U) && n.nodes[0].type == NR_ROUTER && n.nodes[6].type == NR_HOST);
    /* Disconnected network: validation reports it and the ARQ engine refuses to start. */
    nr_network_init(&n, 1U);
    const int a = nr_add_node(&n, "A", NR_HOST, 0.1F, 0.1F), b = nr_add_node(&n, "B", NR_HOST, 0.9F, 0.9F), r = nr_add_node(&n, "R", NR_ROUTER, 0.5F, 0.5F);
    CHECK(nr_add_link(&n, a, r, true, 1, 1, 10) > 0);
    NrIssue issues[NR_MAX_ISSUES];
    size_t found = nr_validate_network(&n, a, b, issues); bool unreachable = false, isolated = false, components = false;
    for (size_t i = 0; i < found; ++i) {
        if (issues[i].severity == NR_ISSUE_ERROR && strstr(issues[i].message, "unreachable") != NULL) unreachable = true;
        if (strstr(issues[i].message, "has no links") != NULL) isolated = true;
        if (strstr(issues[i].message, "disconnected components") != NULL) components = true;
    }
    CHECK(unreachable && isolated && components);
    NrArqConfig c = base_config(NR_ARQ_GO_BACK_N, 5U, 4U); NrArqSimulation s;
    CHECK(!nr_arq_start(&s, &n, &c, a, b));
    found = nr_validate_network(&n, a, a, issues); CHECK(found > 0U && issues[0].severity == NR_ISSUE_ERROR);
    /* Masked routing avoids blocked links. */
    CHECK(nr_generate_topology(&n, NR_TOPO_RING, 6U, 1U));
    bool blocked[NR_MAX_LINKS] = {false};
    NrRoute direct = nr_find_route(&n, n.nodes[0].id, n.nodes[1].id); CHECK(direct.length == 2U);
    blocked[0] = true;
    NrRoute around = nr_find_route_masked(&n, n.nodes[0].id, n.nodes[1].id, blocked); CHECK(around.reachable && around.length == 6U);
    /* Save/load (v2) keeps jitter, loss and queue capacity; v1 files still load. */
    n.links[2].jitter_ms = 4.5; n.links[3].loss_probability = 0.25; n.nodes[2].queue_capacity = 3U; n.links[1].active = false;
    char *text = nr_topology_to_text(&n); CHECK(text != NULL && strncmp(text, "NETRESCUE 2", 11) == 0);
    NrNetwork restored; nr_network_init(&restored, 3U);
    CHECK(nr_topology_from_text(&restored, text));
    CHECK(restored.node_count == n.node_count && restored.link_count == n.link_count);
    CHECK(fabs(restored.links[2].jitter_ms - 4.5) < 1e-9 && fabs(restored.links[3].loss_probability - 0.25) < 1e-9);
    CHECK(restored.nodes[2].queue_capacity == 3U && !restored.links[1].active);
    free(text);
    CHECK(nr_topology_from_text(&restored, "NETRESCUE 1\nNODE 1 0 0.1 0.5 1 H1 10.0.0.1\nNODE 2 0 0.9 0.5 1 H2 10.0.0.2\nLINK 1 1 2 1 1 1 10 100 0\n"));
    CHECK(restored.node_count == 2U && restored.link_count == 1U && restored.nodes[0].queue_capacity == 10U);
    CHECK(!nr_topology_from_text(&restored, "NETRESCUE 3\n"));
    CHECK(!nr_topology_from_text(&restored, "NETRESCUE 2\nLINK 1 1 2 1 1 1 10 100 0 0\n")); /* link to missing nodes */
    CHECK(restored.node_count == 2U); /* failed load leaves the network untouched */
}

int main(void) {
    test_clean_transfers();
    test_window_limits();
    test_gbn_vs_sr_middle_loss();
    test_corruption_detection_and_recovery();
    test_ack_loss_and_duplicates();
    test_retry_limit_and_timeouts();
    test_burst_and_link_outage();
    test_queue_overflow_and_congestion();
    test_random_faults_all_protocols();
    test_reproducibility_and_isolation();
    test_all_presets_run();
    test_step_event_and_controls();
    test_topologies_validation_and_persistence();
    printf("All NetRescue engine tests passed (%d checks).\n", checks);
    return 0;
}
