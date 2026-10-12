#include "experiment.h"
#include "topology.h"
#include <string.h>

/* The seven documented, reproducible experiments required by the lab. Each one builds its own
 * topology and specification, so it never depends on (or changes) the user's working network. */

static const NrPresetInfo PRESETS[] = {
    {"ideal", "1. Ideal network",
     "Baseline with no injected loss or corruption: shows the pure effect of windowing.",
     "8-node ring (H1 -> H5, 4 hops), every link 10 Mbit/s and 5 ms; 100 packets x 1000 B; window 8; timeout 200 ms; 500 pkt/s offered; all three protocols; 1 run (no randomness involved).",
     "Delivery ratio is 100% for all protocols and there are zero retransmissions. Stop-and-Wait is limited to one packet per round trip, so its completion time is several times longer than the windowed protocols."},
    {"loss_sweep", "2. Increasing data loss",
     "Random data-frame loss at 0%, 5%, 10%, 20% and 30% with 10 seeded trials per point.",
     "Same ring as experiment 1; 100 packets; window 8; timeout 200 ms; data loss swept over {0, 0.05, 0.10, 0.20, 0.30}; 10 trials with seed_i = 1000 + 7919 i (identical seeds for every protocol).",
     "Goodput falls and completion time rises with loss. Go-Back-N's retransmission count grows much faster than Selective Repeat's because each loss also resends the frames that followed it."},
    {"corruption", "3. Data corruption",
     "Deterministic corruption of DATA seq 5 on its first transmission, plus random corruption at 0-20%.",
     "Same ring; 100 packets; window 8; timeout 200 ms; NAK enabled; rule corrupt_data(seq 5, attempt 1); corruption swept over {0, 0.05, 0.10, 0.20}; 5 trials; trace captured for trial 0.",
     "Every injected corruption is detected by the FNV-1a checksum and rejected; the receiver NAKs it and the sender recovers. At 0% random corruption only seq 5 is corrupted - look for it in the captured trace."},
    {"ack_loss", "4. ACK loss",
     "Deterministic loss of the first ACK for seq 3, then ACK loss rising from 0% to 30%.",
     "Same ring; 100 packets; window 8; timeout 200 ms; rule drop_ack(seq 3, first ACK); ACK loss swept over {0, 0.05, 0.10, 0.20, 0.30}; 5 trials; trace captured.",
     "Data reached the receiver but the ACK was lost, so Stop-and-Wait and Selective Repeat time out and resend a packet the receiver already has (counted as an unnecessary retransmission and a duplicate, never delivered twice). Go-Back-N's next cumulative ACK usually covers the loss with no retransmission."},
    {"gbn_vs_sr", "5. Go-Back-N versus Selective Repeat",
     "Lose a middle packet (seq 4) once while seqs 5-11 are in flight, and compare the recovery.",
     "Line H1-R2-R3-H4, 10 Mbit/s and 10 ms per link; 24 packets x 1000 B; window 8 for both; timeout 150 ms; 1000 pkt/s offered; rule drop_data(seq 4, attempt 1); 1 run; trace captured.",
     "Verify in the captured trace: Go-Back-N resends seq 4 and every later outstanding frame (4..11) because its receiver discarded them as out of order; Selective Repeat buffers 5..11 and resends only seq 4."},
    {"congestion", "6. Congestion",
     "Bounded queues behind a 1 Mbit/s bottleneck with high offered load, with and without congestion control.",
     "Line H1-R2-R3-H4: H1-R2 and R3-H4 at 100 Mbit/s, R2-R3 at 1 Mbit/s, 5 ms per link; router queues 5 frames; 100 packets x 1000 B; window 16; timeout 300 ms; 1000 pkt/s offered; congestion mode swept over {none, rate AIMD, window AIMD}; 1 run each.",
     "Without congestion control the windowed protocols overflow R2's queue, causing drops, timeouts and long completion times. The AIMD controllers cut the offered load and reduce queue drops."},
    {"timeout", "7. Timeout sensitivity",
     "Timeouts shorter and longer than the round-trip time on a loss-free 4-hop path.",
     "Line of 5 nodes, 10 Mbit/s and 20 ms per link (no-load RTT about 163 ms); 50 packets x 1000 B; window 8; no injected faults; timeout swept over {50, 100, 150, 200, 300, 500} ms; 1 run each.",
     "With a timeout below the RTT every retransmission is unnecessary (the original copy was never lost; it simply had not been acknowledged yet). Once the timeout exceeds the RTT unnecessary retransmissions drop to zero; very long timeouts would only slow recovery from real losses."},
};

size_t nr_preset_count(void) { return sizeof(PRESETS) / sizeof(PRESETS[0]); }
const NrPresetInfo *nr_preset_info(size_t index) { return index < nr_preset_count() ? &PRESETS[index] : NULL; }
int nr_preset_find(const char *id) {
    if (id == NULL) return -1;
    for (size_t i = 0; i < nr_preset_count(); ++i) if (strcmp(PRESETS[i].id, id) == 0) return (int)i;
    return -1;
}

static void set_all_links(NrNetwork *n, double delay_ms, double bandwidth_mbps) {
    for (size_t j = 0; j < n->link_count; ++j) { n->links[j].delay_ms = delay_ms; n->links[j].bandwidth_mbps = bandwidth_mbps; }
}

static void add_rule(NrExperimentSpec *spec, NrFaultRuleType type, unsigned sequence, unsigned attempt) {
    NrFaultRule rule; memset(&rule, 0, sizeof(rule));
    rule.type = type; rule.sequence = sequence; rule.attempt = attempt; rule.link_id = -1; rule.node_id = -1;
    (void)nr_scenario_add(&spec->scenario, &rule);
}

static void sweep(NrExperimentSpec *spec, NrSweepParameter p, const double *values, size_t count) {
    spec->sweep = p; spec->sweep_count = count;
    for (size_t i = 0; i < count; ++i) spec->sweep_values[i] = values[i];
}

static bool ring_base(NrNetwork *n, NrExperimentSpec *spec) {
    if (!nr_generate_topology(n, NR_TOPO_RING, 8U, 1U)) return false;
    set_all_links(n, 5.0, 10.0);
    spec->config.packet_count = 100U; spec->config.payload_bytes = 1000U; spec->config.window_size = 8U;
    spec->config.timeout_seconds = 0.2; spec->config.packets_per_second = 500.0;
    return true;
}

bool nr_preset_build(size_t index, NrNetwork *n, NrExperimentSpec *spec) {
    if (n == NULL || spec == NULL || index >= nr_preset_count()) return false;
    nr_experiment_default_spec(spec);
    spec->config.max_retries = 15U; spec->config.use_nak = true; spec->seed_base = 1000U; spec->seed_stride = 7919U;
    bool ok = true;
    switch (index) {
    case 0: ok = ring_base(n, spec); break;
    case 1: {
        static const double values[] = {0.0, 0.05, 0.10, 0.20, 0.30};
        ok = ring_base(n, spec); sweep(spec, NR_SWEEP_LOSS, values, 5U); spec->trials = 10U; break;
    }
    case 2: {
        static const double values[] = {0.0, 0.05, 0.10, 0.20};
        ok = ring_base(n, spec); add_rule(spec, NR_RULE_CORRUPT_DATA, 5U, 1U);
        sweep(spec, NR_SWEEP_CORRUPTION, values, 4U); spec->trials = 5U; spec->capture_trace = true; break;
    }
    case 3: {
        static const double values[] = {0.0, 0.05, 0.10, 0.20, 0.30};
        ok = ring_base(n, spec); add_rule(spec, NR_RULE_DROP_ACK, 3U, 1U);
        sweep(spec, NR_SWEEP_ACK_LOSS, values, 5U); spec->trials = 5U; spec->capture_trace = true; break;
    }
    case 4:
        ok = nr_generate_topology(n, NR_TOPO_LINE, 4U, 1U);
        set_all_links(n, 10.0, 10.0);
        spec->config.packet_count = 24U; spec->config.payload_bytes = 1000U; spec->config.window_size = 8U;
        spec->config.timeout_seconds = 0.15; spec->config.packets_per_second = 1000.0;
        add_rule(spec, NR_RULE_DROP_DATA, 4U, 1U);
        spec->protocol_mask = (1U << NR_ARQ_GO_BACK_N) | (1U << NR_ARQ_SELECTIVE_REPEAT);
        spec->capture_trace = true; break;
    case 5: {
        static const double values[] = {NR_CC_NONE, NR_CC_RATE_AIMD, NR_CC_WINDOW_AIMD};
        ok = nr_generate_topology(n, NR_TOPO_LINE, 4U, 1U);
        set_all_links(n, 5.0, 100.0);
        if (ok && n->link_count >= 3U) n->links[1].bandwidth_mbps = 1.0;
        for (size_t i = 0; i < n->node_count; ++i) n->nodes[i].queue_capacity = 5U;
        spec->config.packet_count = 100U; spec->config.payload_bytes = 1000U; spec->config.window_size = 16U;
        spec->config.timeout_seconds = 0.3; spec->config.packets_per_second = 1000.0;
        sweep(spec, NR_SWEEP_CONGESTION_MODE, values, 3U); break;
    }
    case 6: {
        static const double values[] = {0.05, 0.10, 0.15, 0.20, 0.30, 0.50};
        ok = nr_generate_topology(n, NR_TOPO_LINE, 5U, 1U);
        set_all_links(n, 20.0, 10.0);
        spec->config.packet_count = 50U; spec->config.payload_bytes = 1000U; spec->config.window_size = 8U;
        spec->config.timeout_seconds = 0.2; spec->config.packets_per_second = 500.0;
        sweep(spec, NR_SWEEP_TIMEOUT, values, 6U); break;
    }
    default: ok = false;
    }
    if (!ok) return false;
    return nr_default_endpoints(n, &spec->source, &spec->destination);
}
