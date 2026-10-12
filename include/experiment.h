#ifndef NETRESCUE_EXPERIMENT_H
#define NETRESCUE_EXPERIMENT_H

/*
 * Fair protocol-comparison runner.
 *
 * For every sweep value x protocol x trial the runner copies the network, applies the same overrides,
 * builds a fresh NrArqSimulation (all protocol, packet, queue and metric state reset) and runs it to
 * completion or to the stopping criteria. Trial i of every protocol uses seed_base + i * seed_stride,
 * so all protocols face the same impairment draws for the same (sequence, attempt) pairs.
 */

#include "arq.h"

#define NR_EXP_MAX_SWEEP 12
#define NR_EXP_MAX_TRIALS 200
#define NR_EXP_MAX_FINDINGS 48
#define NR_EXP_FINDING_LEN 320
#define NR_EXP_RETX_EVIDENCE 128

typedef enum {
    NR_SWEEP_NONE, NR_SWEEP_LOSS, NR_SWEEP_CORRUPTION, NR_SWEEP_ACK_LOSS, NR_SWEEP_ACK_CORRUPTION, NR_SWEEP_TIMEOUT,
    NR_SWEEP_WINDOW, NR_SWEEP_RATE, NR_SWEEP_QUEUE_CAPACITY, NR_SWEEP_DELAY, NR_SWEEP_BANDWIDTH, NR_SWEEP_JITTER,
    NR_SWEEP_CONGESTION_MODE, NR_SWEEP_PAYLOAD, NR_SWEEP_COUNT
} NrSweepParameter;

typedef struct {
    NrArqConfig config;
    NrFaultScenario scenario;
    unsigned protocol_mask;        /* bit (1 << protocol) */
    unsigned trials;
    unsigned seed_base, seed_stride;
    NrSweepParameter sweep;
    double sweep_values[NR_EXP_MAX_SWEEP];
    size_t sweep_count;
    double link_delay_ms, link_bandwidth_mbps, link_jitter_ms; /* < 0 keeps each link's own value */
    int queue_capacity;                                        /* < 0 keeps each node's own value */
    bool capture_trace;            /* keep trace + fault ledger of trial 0 at every sweep point */
    int source, destination;
} NrExperimentSpec;

typedef struct { unsigned sequence, count; } NrRetxEvidence;

typedef struct {
    unsigned sweep_index, trial, seed;
    double sweep_value;
    NrArqProtocol protocol;
    NrArqMetrics metrics;
    NrRetxEvidence retransmitted[NR_EXP_RETX_EVIDENCE]; /* packets with attempts > 1 (from sender state) */
    size_t retransmitted_count, retransmitted_total_sequences;
    NrArqTrace *trace; size_t trace_count;
    NrArqFault *faults; size_t fault_count;
} NrExperimentRun;

typedef enum {
    NR_AGG_DELIVERY_RATIO, NR_AGG_GOODPUT, NR_AGG_THROUGHPUT, NR_AGG_DATA_THROUGHPUT, NR_AGG_LATENCY_AVG, NR_AGG_LATENCY_MAX,
    NR_AGG_COMPLETION_TIME, NR_AGG_RETRANSMISSIONS, NR_AGG_RETX_RATE, NR_AGG_EFFICIENCY, NR_AGG_FRAME_EFFICIENCY,
    NR_AGG_TIMEOUTS, NR_AGG_RETX_UNNECESSARY, NR_AGG_RETX_GOBACK, NR_AGG_DATA_TRANSMISSIONS, NR_AGG_ACKS_SENT,
    NR_AGG_FAULTS_INJECTED, NR_AGG_FAULTS_RECOVERED, NR_AGG_FAULTS_UNRECOVERED, NR_AGG_DATA_LOSSES, NR_AGG_CORRUPTIONS_DETECTED,
    NR_AGG_ACK_LOSSES, NR_AGG_QUEUE_AVG, NR_AGG_QUEUE_MAX, NR_AGG_QUEUE_DROPS, NR_AGG_CONGESTION_DECREASES,
    NR_AGG_UNDELIVERED, NR_AGG_DUPLICATES, NR_AGG_COUNT
} NrAggregateMetric;

typedef struct { double mean, stddev, ci95, min, max; unsigned n; } NrStat;

typedef struct {
    unsigned sweep_index;
    double sweep_value;
    NrArqProtocol protocol;
    unsigned runs, completed;
    NrStat stats[NR_AGG_COUNT];
} NrExperimentAggregate;

typedef struct {
    NrExperimentSpec spec;
    double base_rtt_ms;             /* no-load RTT of the route: data one way + ACK back, incl. serialisation */
    NrRoute route;
    NrExperimentRun *runs; size_t run_count;
    NrExperimentAggregate *aggregates; size_t aggregate_count;
    char findings[NR_EXP_MAX_FINDINGS][NR_EXP_FINDING_LEN]; size_t finding_count;
    char error[160];
} NrExperimentResult;

void nr_experiment_default_spec(NrExperimentSpec *spec);
bool nr_experiment_run(const NrNetwork *network, const NrExperimentSpec *spec, NrExperimentResult *result);
void nr_experiment_free(NrExperimentResult *result);
const char *nr_sweep_name(NrSweepParameter parameter);
bool nr_sweep_from_name(const char *name, NrSweepParameter *parameter);
const char *nr_aggregate_name(NrAggregateMetric metric);
double nr_t_critical_95(unsigned degrees_of_freedom);
double nr_route_base_rtt_ms(const NrNetwork *network, const NrRoute *forward, const NrRoute *reverse, unsigned payload_bytes);
/* Writes a CSV of every run (one row per run). Returns malloc'ed text. */
char *nr_experiment_csv(const NrExperimentResult *result);

/* ---- Prebuilt, documented experiments ---- */
typedef struct { const char *id, *title, *purpose, *settings, *what_to_look_for; } NrPresetInfo;
size_t nr_preset_count(void);
const NrPresetInfo *nr_preset_info(size_t index);
int nr_preset_find(const char *id);
bool nr_preset_build(size_t index, NrNetwork *network, NrExperimentSpec *spec);

#endif
