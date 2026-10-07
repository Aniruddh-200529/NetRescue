#include "arq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NR_ARQ_MAX_ATTEMPTS 16U

static bool event_chance(const NrArqSimulation *s, double probability, unsigned sequence,
                         unsigned attempt, unsigned kind) {
    if (probability <= 0.0) return false;
    if (probability >= 1.0) return true;
    uint32_t value = (uint32_t)s->config.seed ^ (sequence + 1U) * 0x9e3779b9U ^
                     (attempt + 1U) * 0x85ebca6bU ^ (kind + 1U) * 0xc2b2ae35U;
    value ^= value >> 16U; value *= 0x7feb352dU; value ^= value >> 15U;
    value *= 0x846ca68bU; value ^= value >> 16U;
    return (double)(value % 1000000U) / 1000000.0 < probability;
}

void nr_arq_log(NrArqSimulation *s, const char *message) {
    if (s == NULL || message == NULL) return;
    (void)snprintf(s->log[s->log_cursor], NR_EVENT_LEN, "[%06.2fs] %s", s->time_seconds, message);
    s->log_cursor = (s->log_cursor + 1U) % 128U;
    if (s->log_count < 128U) ++s->log_count;
}

static double path_delay(const NrNetwork *n, const NrRoute *route, unsigned payload_bytes) {
    if (n == NULL || route == NULL || !route->reachable || route->length == 0U) return INFINITY;
    double ms = 0.0;
    for (size_t i = 0; i + 1U < route->length; ++i) {
        const int from = route->nodes[i], to = route->nodes[i + 1U];
        const NrLink *found = NULL;
        for (size_t j = 0; j < n->link_count; ++j) {
            const NrLink *link = &n->links[j];
            if (link->active && ((link->from == from && link->to == to) || (link->bidirectional && link->from == to && link->to == from))) { found = link; break; }
        }
        if (found == NULL) return INFINITY;
        ms += found->delay_ms;
        if (found->bandwidth_mbps > 0.0) ms += (double)payload_bytes * 8.0 / (found->bandwidth_mbps * 1000.0);
    }
    return ms / 1000.0;
}

static double route_loss(const NrNetwork *n, const NrRoute *route) {
    double success = 1.0;
    if (n == NULL || route == NULL) return 1.0;
    for (size_t i = 0; i + 1U < route->length; ++i) {
        const int from = route->nodes[i], to = route->nodes[i + 1U];
        for (size_t j = 0; j < n->link_count; ++j) {
            const NrLink *link = &n->links[j];
            if ((link->from == from && link->to == to) || (link->bidirectional && link->from == to && link->to == from)) {
                const double loss = isfinite(link->loss_probability) ? fmax(0.0, fmin(1.0, link->loss_probability)) : 1.0;
                success *= 1.0 - loss; break;
            }
        }
    }
    return 1.0 - success;
}

static double combine_probability(double a, double b) { return 1.0 - (1.0 - a) * (1.0 - b); }

static bool enqueue_event(NrArqSimulation *s, NrArqEvent event) {
    if (s->event_count >= NR_ARQ_MAX_EVENTS) {
        s->state = NR_ARQ_FAILED;
        nr_arq_log(s, "Simulation stopped: event capacity reached");
        return false;
    }
    s->events[s->event_count++] = event;
    return true;
}

static size_t route_queue_capacity(const NrArqSimulation *s) {
    size_t capacity = NR_ARQ_MAX_PACKETS;
    for (size_t i = 1U; i + 1U < s->forward_route.length; ++i) {
        NrNode *node = nr_find_node((NrNetwork *)s->network, s->forward_route.nodes[i]);
        if (node != NULL && (node->type == NR_ROUTER || node->type == NR_SWITCH) && node->queue_capacity < capacity)
            capacity = node->queue_capacity;
    }
    return capacity;
}

static size_t route_queue_used(const NrArqSimulation *s) {
    size_t used = 0U;
    for (size_t i = 0; i < s->event_count; ++i)
        if (s->events[i].type == NR_ARQ_DATA_EVENT && !s->events[i].lost) ++used;
    return used;
}

static void set_event_checksum(NrArqEvent *event, uint32_t value, bool corrupt) {
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8U), (unsigned char)(value >> 16U), (unsigned char)(value >> 24U)};
    event->checksum = nr_checksum(bytes, sizeof(bytes));
    if (corrupt) bytes[event->sequence % 4U] ^= 0x01U;
    event->received_checksum = nr_checksum(bytes, sizeof(bytes));
    event->corrupted = event->checksum != event->received_checksum;
}

static void advance_sender_base(NrArqSimulation *s) {
    while (s->sender_base < s->config.packet_count && s->frames[s->sender_base].acknowledged) ++s->sender_base;
    if (s->sender_base >= s->config.packet_count && s->next_sequence >= s->config.packet_count) {
        s->state = NR_ARQ_COMPLETE;
        nr_arq_log(s, "All packets acknowledged; transfer complete");
    }
}

static bool transmit(NrArqSimulation *s, size_t sequence, bool retransmission) {
    if (sequence >= s->config.packet_count) return false;
    NrArqFrame *frame = &s->frames[sequence];
    if (frame->acknowledged || frame->failed) return false;
    if (frame->attempts >= NR_ARQ_MAX_ATTEMPTS) {
        frame->failed = true;
        s->state = NR_ARQ_FAILED;
        nr_arq_log(s, "Transfer failed: retry limit reached");
        return false;
    }
    if (retransmission) ++s->stats.retransmissions;
    else { frame->created_at = s->time_seconds; ++s->stats.generated; }
    ++frame->attempts;
    frame->last_sent_at = s->time_seconds;
    ++s->stats.data_transmissions;
    NrArqEvent event; memset(&event, 0, sizeof(event));
    event.type = NR_ARQ_DATA_EVENT; event.sequence = (unsigned)sequence; event.attempt = frame->attempts;
    event.started_at = s->time_seconds; event.due_at = s->time_seconds + s->forward_delay_seconds;
    const double loss = combine_probability(s->config.loss_probability, route_loss(s->network, &s->forward_route));
    const size_t queue_capacity = route_queue_capacity(s);
    const bool queue_full = queue_capacity == 0U || route_queue_used(s) >= queue_capacity;
    event.congestion_drop = queue_full;
    event.lost = queue_full || event_chance(s, loss, (unsigned)sequence, frame->attempts, 0U);
    if (queue_full) {
        ++s->stats.queue_drops;
        s->queue_utilization = 1.0;
        if (s->config.congestion_control) {
            (void)nr_congestion_observe(&s->congestion, 0.01, 1.0, true);
            nr_arq_log(s, "Congestion detected: multiplicative traffic-rate decrease");
        }
    }
    set_event_checksum(&event, sequence ^ s->config.seed, !event.lost && event_chance(s, s->config.corruption_probability, (unsigned)sequence, frame->attempts, 1U));
    char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), "%s data #%zu (attempt %u)", retransmission ? "Retransmitting" : "Sending", sequence + 1U, frame->attempts); nr_arq_log(s, text);
    return enqueue_event(s, event);
}

static void schedule_ack(NrArqSimulation *s, unsigned sequence, unsigned ack_number, bool negative) {
    NrArqEvent event; memset(&event, 0, sizeof(event));
    event.type = NR_ARQ_ACK_EVENT; event.sequence = sequence; event.ack_number = ack_number; event.negative = negative;
    event.started_at = s->time_seconds; event.due_at = s->time_seconds + s->reverse_delay_seconds;
    const double loss = combine_probability(s->config.ack_loss_probability, route_loss(s->network, &s->reverse_route));
    const unsigned attempt = sequence < s->config.packet_count ? s->frames[sequence].attempts : 0U;
    event.lost = event_chance(s, loss, sequence, attempt, 2U);
    set_event_checksum(&event, ack_number ^ sequence, !event.lost && event_chance(s, s->config.ack_corruption_probability, sequence, attempt, 3U));
    (void)enqueue_event(s, event);
}

static void receive_data(NrArqSimulation *s, const NrArqEvent *event) {
    const size_t seq = event->sequence;
    if (event->lost) {
        ++s->stats.packet_losses;
        char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), event->congestion_drop ? "Packet #%zu dropped: forwarding queue full" : "Packet #%zu lost in transit", seq + 1U); nr_arq_log(s, text);
        return;
    }
    if (event->corrupted) {
        ++s->stats.corruptions;
        char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), "Checksum error on packet #%zu; NACK sent", seq + 1U); nr_arq_log(s, text);
        schedule_ack(s, (unsigned)seq, (unsigned)seq, true);
        return;
    }

    const bool selective = s->config.protocol == NR_ARQ_SELECTIVE_REPEAT;
    if (selective) {
        if (seq < s->receiver_base || s->receiver_buffer[seq]) {
            ++s->stats.duplicates;
            char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), "Duplicate packet #%zu acknowledged", seq + 1U); nr_arq_log(s, text);
        } else if (seq >= s->receiver_base + s->config.window_size) {
            ++s->stats.out_of_order;
            nr_arq_log(s, "Out-of-window packet discarded");
            return;
        } else {
            if (seq > s->receiver_base) ++s->stats.out_of_order;
            s->receiver_buffer[seq] = true;
            NrArqFrame *frame = &s->frames[seq]; frame->received = true; frame->delivered_at = s->time_seconds;
            ++s->stats.delivered;
            const double latency = (s->time_seconds - frame->created_at) * 1000.0;
            s->stats.total_latency_ms += latency; if (latency > s->stats.max_latency_ms) s->stats.max_latency_ms = latency;
            while (s->receiver_base < s->config.packet_count && s->receiver_buffer[s->receiver_base]) ++s->receiver_base;
            char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), "Packet #%zu received and buffered; ACK sent", seq + 1U); nr_arq_log(s, text);
        }
        schedule_ack(s, (unsigned)seq, (unsigned)seq, false);
        return;
    }

    if (seq == s->receiver_base) {
        s->receiver_buffer[seq] = true;
        NrArqFrame *frame = &s->frames[seq]; frame->received = true; frame->delivered_at = s->time_seconds;
        ++s->stats.delivered;
        const double latency = (s->time_seconds - frame->created_at) * 1000.0;
        s->stats.total_latency_ms += latency; if (latency > s->stats.max_latency_ms) s->stats.max_latency_ms = latency;
        while (s->receiver_base < s->config.packet_count && s->receiver_buffer[s->receiver_base]) ++s->receiver_base;
        char text[NR_EVENT_LEN]; (void)snprintf(text, sizeof(text), "Packet #%zu delivered in order; cumulative ACK sent", seq + 1U); nr_arq_log(s, text);
    } else if (seq < s->receiver_base) {
        ++s->stats.duplicates;
        nr_arq_log(s, "Duplicate data received; cumulative ACK resent");
    } else {
        ++s->stats.out_of_order;
        nr_arq_log(s, "Out-of-order data discarded; cumulative ACK resent");
    }
    schedule_ack(s, (unsigned)seq, (unsigned)s->receiver_base, false);
}

static void retransmit_from_base(NrArqSimulation *s) {
    const size_t end = s->next_sequence;
    for (size_t i = s->sender_base; i < end && s->state == NR_ARQ_RUNNING; ++i) if (!s->frames[i].acknowledged) (void)transmit(s, i, true);
}

static void receive_ack(NrArqSimulation *s, const NrArqEvent *event) {
    if (event->lost) { ++s->stats.ack_losses; nr_arq_log(s, "ACK lost on return path"); return; }
    if (event->corrupted) { ++s->stats.ack_corruptions; nr_arq_log(s, "Corrupted ACK discarded; sender will timeout"); return; }
    if (event->negative) {
        ++s->stats.negative_acks;
        nr_arq_log(s, "NACK received; retransmission requested");
        if (s->config.protocol == NR_ARQ_GO_BACK_N) retransmit_from_base(s);
        else (void)transmit(s, event->sequence, true);
        return;
    }
    ++s->stats.acknowledgements;
    if (s->config.protocol == NR_ARQ_GO_BACK_N) {
        size_t ack_number = event->ack_number;
        if (ack_number > s->next_sequence) ack_number = s->next_sequence;
        for (size_t i = s->sender_base; i < ack_number; ++i) s->frames[i].acknowledged = true;
    } else if (event->sequence < s->config.packet_count) {
        s->frames[event->sequence].acknowledged = true;
    }
    advance_sender_base(s);
}

static void process_due_events(NrArqSimulation *s) {
    size_t i = 0U;
    while (i < s->event_count && s->state == NR_ARQ_RUNNING) {
        if (s->events[i].due_at > s->time_seconds) { ++i; continue; }
        NrArqEvent event = s->events[i];
        s->events[i] = s->events[--s->event_count];
        if (event.type == NR_ARQ_DATA_EVENT) receive_data(s, &event); else receive_ack(s, &event);
    }
}

static void check_timeouts(NrArqSimulation *s) {
    if (s->config.protocol == NR_ARQ_GO_BACK_N) {
        if (s->sender_base < s->next_sequence && !s->frames[s->sender_base].acknowledged &&
            s->time_seconds - s->frames[s->sender_base].last_sent_at >= s->config.timeout_seconds) {
            ++s->stats.timeouts; nr_arq_log(s, "Go-Back-N base timeout; retransmitting outstanding window"); retransmit_from_base(s);
        }
        return;
    }
    for (size_t i = s->sender_base; i < s->next_sequence && s->state == NR_ARQ_RUNNING; ++i) {
        NrArqFrame *frame = &s->frames[i];
        if (!frame->acknowledged && !frame->failed && s->time_seconds - frame->last_sent_at >= s->config.timeout_seconds) {
            ++s->stats.timeouts;
            nr_arq_log(s, s->config.protocol == NR_ARQ_STOP_WAIT ? "Stop-and-Wait timeout" : "Selective Repeat packet timeout");
            (void)transmit(s, i, true);
        }
    }
}

static void sample_metrics(NrArqSimulation *s) {
    NrArqSample sample;
    sample.time_seconds = s->time_seconds; sample.delivery_ratio = nr_arq_delivery_ratio(s);
    sample.throughput_mbps = nr_arq_throughput_mbps(s);
    sample.average_latency_ms = s->stats.delivered == 0U ? 0.0 : s->stats.total_latency_ms / (double)s->stats.delivered;
    sample.queue_utilization = s->queue_utilization;
    sample.packet_loss_rate = s->stats.data_transmissions == 0U ? 0.0 : (double)s->stats.packet_losses / (double)s->stats.data_transmissions;
    if (s->history_count == NR_ARQ_HISTORY) {
        memmove(s->history, s->history + 1, (NR_ARQ_HISTORY - 1U) * sizeof(s->history[0])); --s->history_count;
    }
    s->history[s->history_count++] = sample;
}

bool nr_arq_start(NrArqSimulation *s, const NrNetwork *network, const NrArqConfig *config, int source, int destination) {
    if (s == NULL || network == NULL || config == NULL || config->protocol < NR_ARQ_STOP_WAIT || config->protocol >= NR_ARQ_PROTOCOL_COUNT ||
        config->packet_count == 0U || config->packet_count > NR_ARQ_MAX_PACKETS || config->window_size == 0U || config->window_size > NR_ARQ_MAX_PACKETS ||
        config->payload_bytes == 0U || !isfinite(config->packets_per_second) || config->packets_per_second < 0.0 ||
        !isfinite(config->timeout_seconds) || config->timeout_seconds <= 0.0 ||
        !isfinite(config->loss_probability) || config->loss_probability < 0.0 || config->loss_probability > 1.0 ||
        !isfinite(config->corruption_probability) || config->corruption_probability < 0.0 || config->corruption_probability > 1.0 ||
        !isfinite(config->ack_loss_probability) || config->ack_loss_probability < 0.0 || config->ack_loss_probability > 1.0 ||
        !isfinite(config->ack_corruption_probability) || config->ack_corruption_probability < 0.0 || config->ack_corruption_probability > 1.0) return false;

    NrRoute forward = nr_find_route(network, source, destination), reverse = nr_find_route(network, destination, source);
    if (!forward.reachable || !reverse.reachable) return false;
    memset(s, 0, sizeof(*s)); s->network = network; s->config = *config;
    s->source = source; s->destination = destination;
    if (s->config.protocol == NR_ARQ_STOP_WAIT) s->config.window_size = 1U;
    s->forward_route = forward; s->reverse_route = reverse;
    s->forward_delay_seconds = fmax(0.001, path_delay(network, &forward, config->payload_bytes));
    s->reverse_delay_seconds = fmax(0.001, path_delay(network, &reverse, 8U));
    s->rng_state = config->seed == 0U ? 1U : config->seed;
    nr_congestion_init(&s->congestion, config->packets_per_second);
    s->state = NR_ARQ_RUNNING; s->next_generation_at = 0.0; s->next_sample_at = 0.1;
    nr_arq_log(s, "Reliable transfer started");
    sample_metrics(s);
    return true;
}

void nr_arq_step(NrArqSimulation *s, double dt) {
    if (s == NULL || s->state != NR_ARQ_RUNNING || !isfinite(dt) || dt <= 0.0) return;
    s->time_seconds += dt;
    NrRoute forward = nr_find_route(s->network, s->source, s->destination), reverse = nr_find_route(s->network, s->destination, s->source);
    if (!forward.reachable || !reverse.reachable) { s->state = NR_ARQ_FAILED; nr_arq_log(s, "Route failure: transfer has no forward or ACK path"); return; }
    bool changed = forward.length != s->forward_route.length || reverse.length != s->reverse_route.length;
    if (!changed) {
        for (size_t i = 0; i < forward.length; ++i) if (forward.nodes[i] != s->forward_route.nodes[i]) { changed = true; break; }
        for (size_t i = 0; !changed && i < reverse.length; ++i) if (reverse.nodes[i] != s->reverse_route.nodes[i]) { changed = true; break; }
    }
    if (changed) {
        s->forward_route = forward; s->reverse_route = reverse;
        s->forward_delay_seconds = fmax(0.001, path_delay(s->network, &forward, s->config.payload_bytes));
        s->reverse_delay_seconds = fmax(0.001, path_delay(s->network, &reverse, 8U));
        nr_arq_log(s, "Topology changed; forward and ACK routes recalculated");
        for (size_t i = 0; i < s->event_count; ++i) {
            NrArqEvent *event = &s->events[i];
            if (event->due_at <= s->time_seconds) continue;
            event->started_at = s->time_seconds;
            event->due_at = s->time_seconds + (event->type == NR_ARQ_ACK_EVENT ? s->reverse_delay_seconds : s->forward_delay_seconds);
        }
        nr_arq_log(s, "In-flight data and ACK events rerouted on the active graph");
    }
    process_due_events(s);
    if (s->state != NR_ARQ_RUNNING) return;
    const size_t queue_capacity = route_queue_capacity(s), queue_used = route_queue_used(s);
    s->queue_utilization = queue_capacity == 0U ? 1.0 : fmin(1.0, (double)queue_used / (double)queue_capacity);
    if (s->config.congestion_control)
        (void)nr_congestion_observe(&s->congestion, dt, s->queue_utilization, false);
    check_timeouts(s);
    const unsigned window = s->config.protocol == NR_ARQ_STOP_WAIT ? 1U : s->config.window_size;
    if (s->config.packets_per_second > 0.0) {
        const double generation_rate = s->config.congestion_control ? s->congestion.rate : s->config.packets_per_second;
        while (generation_rate > 0.0 && s->next_sequence < s->config.packet_count && s->next_sequence < s->sender_base + window &&
               s->time_seconds >= s->next_generation_at && s->state == NR_ARQ_RUNNING) {
            if (!transmit(s, s->next_sequence, false)) break;
            ++s->next_sequence;
            s->next_generation_at += 1.0 / generation_rate;
        }
    }
    process_due_events(s);
    if (s->time_seconds >= s->next_sample_at) {
        sample_metrics(s);
        s->next_sample_at = s->time_seconds + 0.1;
    }
}

void nr_arq_pause(NrArqSimulation *s) { if (s != NULL && s->state == NR_ARQ_RUNNING) { s->state = NR_ARQ_PAUSED; nr_arq_log(s, "Traffic paused"); } }
void nr_arq_resume(NrArqSimulation *s) { if (s != NULL && s->state == NR_ARQ_PAUSED) { s->state = NR_ARQ_RUNNING; nr_arq_log(s, "Traffic resumed"); } }
void nr_arq_stop(NrArqSimulation *s) { if (s != NULL && (s->state == NR_ARQ_RUNNING || s->state == NR_ARQ_PAUSED)) { s->state = NR_ARQ_STOPPED; nr_arq_log(s, "Transfer stopped"); } }

const char *nr_arq_protocol_name(NrArqProtocol p) { static const char *names[] = {"Stop-and-Wait", "Go-Back-N", "Selective Repeat"}; return p >= NR_ARQ_STOP_WAIT && p < NR_ARQ_PROTOCOL_COUNT ? names[p] : "Unknown"; }
const char *nr_arq_state_name(NrArqState st) { static const char *names[] = {"Idle", "Running", "Paused", "Complete", "Failed", "Stopped"}; return st >= NR_ARQ_IDLE && st <= NR_ARQ_STOPPED ? names[st] : "Unknown"; }
double nr_arq_delivery_ratio(const NrArqSimulation *s) { return s == NULL || s->stats.generated == 0U ? 0.0 : (double)s->stats.delivered / (double)s->stats.generated; }
double nr_arq_throughput_mbps(const NrArqSimulation *s) { return s == NULL || s->time_seconds <= 0.0 ? 0.0 : (double)s->stats.delivered * (double)s->config.payload_bytes * 8.0 / (s->time_seconds * 1000000.0); }
double nr_arq_efficiency(const NrArqSimulation *s) { return s == NULL || s->stats.data_transmissions == 0U ? 0.0 : (double)s->stats.delivered / (double)s->stats.data_transmissions; }

double nr_arq_packet_progress(const NrArqSimulation *s, unsigned sequence, bool *corrupted) {
    if (corrupted != NULL) *corrupted = false;
    if (s == NULL || sequence >= s->config.packet_count) return 0.0;
    for (size_t i = 0; i < s->event_count; ++i) if (s->events[i].type == NR_ARQ_DATA_EVENT && s->events[i].sequence == sequence) {
        if (corrupted != NULL) *corrupted = s->events[i].corrupted;
        const double duration = s->events[i].due_at - s->events[i].started_at;
        return duration <= 0.0 ? 1.0 : fmax(0.0, fmin(1.0, (s->time_seconds - s->events[i].started_at) / duration));
    }
    return 0.0;
}

bool nr_arq_compare(const NrNetwork *network, const NrArqConfig *config, int source, int destination,
                    NrArqComparisonResult *go_back_n, NrArqComparisonResult *selective_repeat) {
    if (network == NULL || config == NULL || go_back_n == NULL || selective_repeat == NULL ||
        config->packets_per_second <= 0.0 || config->packet_count == 0U) return false;
    NrArqProtocol protocols[2] = {NR_ARQ_GO_BACK_N, NR_ARQ_SELECTIVE_REPEAT};
    NrArqComparisonResult *results[2] = {go_back_n, selective_repeat};
    for (size_t run = 0; run < 2U; ++run) {
        NrArqConfig trial = *config;
        trial.protocol = protocols[run];
        NrArqSimulation *simulation = malloc(sizeof(*simulation));
        if (simulation == NULL) return false;
        if (!nr_arq_start(simulation, network, &trial, source, destination)) {
            free(simulation);
            return false;
        }
        for (unsigned step = 0U; step < 300000U && simulation->state == NR_ARQ_RUNNING; ++step)
            nr_arq_step(simulation, 0.01);
        results[run]->completed = simulation->state == NR_ARQ_COMPLETE;
        results[run]->duration_seconds = simulation->time_seconds;
        results[run]->efficiency = nr_arq_efficiency(simulation);
        results[run]->statistics = simulation->stats;
        free(simulation);
    }
    return true;
}
