#ifndef NETRESCUE_ARQ_H
#define NETRESCUE_ARQ_H

#include "netrescue.h"
#include "congestion.h"

#define NR_ARQ_MAX_PACKETS 512
#define NR_ARQ_MAX_EVENTS 4096
#define NR_ARQ_HISTORY 128

typedef enum { NR_ARQ_STOP_WAIT, NR_ARQ_GO_BACK_N, NR_ARQ_SELECTIVE_REPEAT, NR_ARQ_PROTOCOL_COUNT } NrArqProtocol;
typedef enum { NR_ARQ_IDLE, NR_ARQ_RUNNING, NR_ARQ_PAUSED, NR_ARQ_COMPLETE, NR_ARQ_FAILED, NR_ARQ_STOPPED } NrArqState;
typedef enum { NR_ARQ_DATA_EVENT, NR_ARQ_ACK_EVENT } NrArqEventType;

typedef struct {
    NrArqProtocol protocol;
    size_t packet_count;
    unsigned window_size;
    unsigned payload_bytes;
    double packets_per_second;
    double timeout_seconds;
    double loss_probability;
    double corruption_probability;
    double ack_loss_probability;
    double ack_corruption_probability;
    unsigned seed;
    bool congestion_control;
} NrArqConfig;

typedef struct {
    bool acknowledged, received, failed;
    unsigned attempts;
    double created_at, last_sent_at, delivered_at;
} NrArqFrame;

typedef struct {
    NrArqEventType type;
    unsigned sequence, ack_number, attempt;
    double started_at, due_at;
    uint32_t checksum, received_checksum;
    bool lost, corrupted, negative, congestion_drop;
} NrArqEvent;

typedef struct {
    double time_seconds, delivery_ratio, throughput_mbps, average_latency_ms, queue_utilization, packet_loss_rate;
} NrArqSample;

typedef struct {
    uint64_t generated, data_transmissions, delivered, packet_losses, corruptions;
    uint64_t acknowledgements, negative_acks, ack_losses, ack_corruptions;
    uint64_t retransmissions, timeouts, out_of_order, duplicates, queue_drops;
    double total_latency_ms, max_latency_ms;
} NrArqStatistics;

typedef struct {
    bool completed;
    double duration_seconds, efficiency;
    NrArqStatistics statistics;
} NrArqComparisonResult;

typedef struct {
    const NrNetwork *network;
    NrRoute forward_route, reverse_route;
    int source, destination;
    NrArqConfig config;
    NrArqFrame frames[NR_ARQ_MAX_PACKETS];
    bool receiver_buffer[NR_ARQ_MAX_PACKETS];
    NrArqEvent events[NR_ARQ_MAX_EVENTS];
    size_t event_count, next_sequence, sender_base, receiver_base;
    unsigned rng_state;
    double time_seconds, next_generation_at, forward_delay_seconds, reverse_delay_seconds, queue_utilization;
    NrCongestionController congestion;
    NrArqState state;
    NrArqStatistics stats;
    NrArqSample history[NR_ARQ_HISTORY];
    size_t history_count;
    double next_sample_at;
    char log[128][NR_EVENT_LEN];
    size_t log_count, log_cursor;
} NrArqSimulation;

bool nr_arq_start(NrArqSimulation *simulation, const NrNetwork *network,
                  const NrArqConfig *config, int source, int destination);
void nr_arq_step(NrArqSimulation *simulation, double delta_seconds);
void nr_arq_pause(NrArqSimulation *simulation);
void nr_arq_resume(NrArqSimulation *simulation);
void nr_arq_stop(NrArqSimulation *simulation);
void nr_arq_log(NrArqSimulation *simulation, const char *message);
const char *nr_arq_protocol_name(NrArqProtocol protocol);
const char *nr_arq_state_name(NrArqState state);
double nr_arq_delivery_ratio(const NrArqSimulation *simulation);
double nr_arq_throughput_mbps(const NrArqSimulation *simulation);
double nr_arq_efficiency(const NrArqSimulation *simulation);
double nr_arq_packet_progress(const NrArqSimulation *simulation, unsigned sequence, bool *corrupted);
bool nr_arq_compare(const NrNetwork *network, const NrArqConfig *config, int source, int destination,
                    NrArqComparisonResult *go_back_n, NrArqComparisonResult *selective_repeat);

#endif
