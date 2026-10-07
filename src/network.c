#include "netrescue.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned nr_random(NrNetwork *n) {
    n->rng_state = n->rng_state * 1664525U + 1013904223U;
    return n->rng_state;
}

static void release_packet_queue(NrNetwork *n, NrPacket *p) {
    if (!p->queued_at_current) return;
    NrNode *node = nr_find_node(n, p->current_node);
    if (node != NULL && node->queue_size > 0U) --node->queue_size;
    p->queued_at_current = false;
}

void nr_log(NrNetwork *n, const char *message) {
    if (n == NULL || message == NULL) return;
    (void)snprintf(n->events[n->event_cursor], NR_EVENT_LEN, "[%06.2fs] %s", n->time_seconds, message);
    n->event_cursor = (n->event_cursor + 1U) % 64U;
    if (n->event_count < 64U) ++n->event_count;
}

void nr_network_init(NrNetwork *n, unsigned seed) {
    if (n == NULL) return;
    memset(n, 0, sizeof(*n));
    n->next_node_id = 1U; n->next_link_id = 1U; n->next_packet_id = 1U;
    n->rng_state = seed == 0U ? 1U : seed;
    n->cost_mode = NR_COST_LINK; n->weight_delay = 0.1; n->weight_congestion = 2.0;
    nr_log(n, "Network initialized");
}

NrNode *nr_find_node(NrNetwork *n, int id) {
    if (n == NULL) return NULL;
    for (size_t i = 0; i < n->node_count; ++i) if (n->nodes[i].id == id) return &n->nodes[i];
    return NULL;
}
NrLink *nr_find_link(NrNetwork *n, int id) {
    if (n == NULL) return NULL;
    for (size_t i = 0; i < n->link_count; ++i) if (n->links[i].id == id) return &n->links[i];
    return NULL;
}

int nr_add_node(NrNetwork *n, const char *name, NrNodeType type, float x, float y) {
    if (n == NULL || name == NULL || n->node_count >= NR_MAX_NODES || type < NR_HOST || type > NR_AP ||
        !isfinite(x) || !isfinite(y)) return -1;
    NrNode *node = &n->nodes[n->node_count++];
    memset(node, 0, sizeof(*node)); node->id = (int)n->next_node_id++; node->type = type;
    (void)snprintf(node->name, sizeof(node->name), "%s", name);
    node->x = x; node->y = y; node->active = true; node->queue_capacity = 10U;
    (void)snprintf(node->ip, sizeof(node->ip), "192.168.1.%u", (unsigned)node->id % 254U + 1U);
    return node->id;
}

bool nr_remove_node(NrNetwork *n, int id) {
    if (n == NULL) return false;
    NrNode *node = nr_find_node(n, id); if (node == NULL) return false;
    const size_t index = (size_t)(node - n->nodes);
    for (size_t i = n->link_count; i > 0U; --i) {
        NrLink *l = &n->links[i - 1U];
        if (l->from == id || l->to == id) { memmove(l, l + 1, (n->link_count - i) * sizeof(*l)); --n->link_count; }
    }
    memmove(&n->nodes[index], &n->nodes[index + 1U], (n->node_count - index - 1U) * sizeof(*node));
    --n->node_count;
    return true;
}

int nr_add_link(NrNetwork *n, int from, int to, bool bi, double cost, double delay, double bandwidth) {
    if (n == NULL || from == to || nr_find_node(n, from) == NULL || nr_find_node(n, to) == NULL || n->link_count >= NR_MAX_LINKS ||
        !isfinite(cost) || !isfinite(delay) || !isfinite(bandwidth) || cost < 0.0 || delay < 0.0 || bandwidth < 0.0) return -1;
    for (size_t i = 0; i < n->link_count; ++i) {
        const NrLink *l = &n->links[i];
        if (l->from == from && l->to == to && l->bidirectional == bi) return -1;
        if (bi && l->bidirectional && l->from == to && l->to == from) return -1;
    }
    NrLink *l = &n->links[n->link_count++]; memset(l, 0, sizeof(*l));
    l->id = (int)n->next_link_id++; l->from = from; l->to = to; l->bidirectional = bi; l->active = true;
    l->cost = cost; l->delay_ms = delay; l->bandwidth_mbps = bandwidth;
    return l->id;
}

bool nr_remove_link(NrNetwork *n, int id) {
    if (n == NULL) return false;
    NrLink *l = nr_find_link(n, id); if (l == NULL) return false;
    const size_t i = (size_t)(l - n->links);
    memmove(l, l + 1, (n->link_count - i - 1U) * sizeof(*l)); --n->link_count; return true;
}
bool nr_set_node_active(NrNetwork *n, int id, bool active) {
    NrNode *node = nr_find_node(n, id); if (node == NULL) return false;
    node->active = active;
    char msg[NR_EVENT_LEN]; (void)snprintf(msg, sizeof(msg), "Node %s %s", node->name, active ? "recovered" : "failed"); nr_log(n, msg); return true;
}
bool nr_set_link_active(NrNetwork *n, int id, bool active) {
    NrLink *l = nr_find_link(n, id); if (l == NULL) return false;
    l->active = active;
    char msg[NR_EVENT_LEN]; (void)snprintf(msg, sizeof(msg), "Link %d %s; recalculating routes", id, active ? "recovered" : "FAILED"); nr_log(n, msg); return true;
}

static double edge_cost(const NrNetwork *n, const NrLink *l) {
    if (n->cost_mode == NR_COST_DELAY) return l->delay_ms;
    if (n->cost_mode == NR_COST_COMPOSITE) return l->cost + l->delay_ms * n->weight_delay + l->utilization * n->weight_congestion;
    return l->cost;
}

NrRoute nr_find_route(const NrNetwork *n, int source, int destination) {
    NrRoute route; memset(&route, 0, sizeof(route));
    if (n == NULL || nr_find_node((NrNetwork *)n, source) == NULL || nr_find_node((NrNetwork *)n, destination) == NULL) return route;
    if (!nr_find_node((NrNetwork *)n, source)->active || !nr_find_node((NrNetwork *)n, destination)->active) return route;
    double dist[NR_MAX_NODES]; int prev[NR_MAX_NODES]; bool used[NR_MAX_NODES] = {false};
    for (size_t i = 0; i < NR_MAX_NODES; ++i) { dist[i] = INFINITY; prev[i] = -1; }
    size_t src = NR_MAX_NODES, dst = NR_MAX_NODES;
    for (size_t i = 0; i < n->node_count; ++i) { if (n->nodes[i].id == source) src = i; if (n->nodes[i].id == destination) dst = i; }
    if (src == NR_MAX_NODES || dst == NR_MAX_NODES) return route;
    dist[src] = 0.0;
    for (size_t iteration = 0; iteration < n->node_count; ++iteration) {
        size_t u = NR_MAX_NODES; double best = INFINITY;
        for (size_t i = 0; i < n->node_count; ++i) if (!used[i] && dist[i] < best) { best = dist[i]; u = i; }
        if (u == NR_MAX_NODES) break;
        if (u == dst) break;
        used[u] = true;
        for (size_t j = 0; j < n->link_count; ++j) {
            const NrLink *l = &n->links[j]; if (!l->active) continue;
            int next_id = -1;
            if (l->from == n->nodes[u].id) next_id = l->to;
            else if (l->bidirectional && l->to == n->nodes[u].id) next_id = l->from;
            if (next_id < 0) continue;
            size_t v = NR_MAX_NODES;
            for (size_t k = 0; k < n->node_count; ++k) if (n->nodes[k].id == next_id && n->nodes[k].active) { v = k; break; }
            if (v < NR_MAX_NODES && dist[u] + edge_cost(n, l) < dist[v]) { dist[v] = dist[u] + edge_cost(n, l); prev[v] = (int)u; }
        }
    }
    if (!isfinite(dist[dst])) return route;
    int reverse[NR_MAX_HOPS]; size_t count = 0U; int at = (int)dst;
    while (at >= 0 && count < NR_MAX_HOPS) { reverse[count++] = n->nodes[(size_t)at].id; if ((size_t)at == src) break; at = prev[(size_t)at]; }
    if (count == 0U || reverse[count - 1U] != source) return route;
    route.reachable = true; route.length = count; route.cost = dist[dst];
    for (size_t i = 0; i < count; ++i) route.nodes[i] = reverse[count - i - 1U];
    return route;
}
bool nr_is_connected(const NrNetwork *n, int source, int destination) { return nr_find_route(n, source, destination).reachable; }

unsigned nr_checksum(const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data; uint32_t sum = 2166136261U;
    if (bytes == NULL && length != 0U) return 0U;
    for (size_t i = 0; i < length; ++i) { sum ^= bytes[i]; sum *= 16777619U; }
    return sum;
}

int nr_send_packet(NrNetwork *n, int source, int destination, unsigned size) {
    if (n == NULL || n->packet_count >= NR_MAX_PACKETS || nr_find_node(n, source) == NULL || nr_find_node(n, destination) == NULL) return -1;
    NrPacket *p = &n->packets[n->packet_count++]; memset(p, 0, sizeof(*p));
    p->id = n->next_packet_id++; p->sequence = p->id; p->source = source; p->destination = destination;
    p->current_node = source; p->ttl = 64U; p->payload_size = size; p->created_at = n->time_seconds; p->state = NR_PACKET_CREATED;
    const char payload[32] = "NetRescue packet payload"; p->checksum = nr_checksum(payload, size < sizeof(payload) ? size : sizeof(payload));
    ++n->stats.generated;
    NrRoute r = nr_find_route(n, source, destination);
    if (!r.reachable) { p->state = NR_PACKET_UNREACHABLE; ++n->stats.dropped; nr_log(n, "Packet destination unreachable"); return (int)p->id; }
    p->path_length = r.length; memcpy(p->path, r.nodes, r.length * sizeof(r.nodes[0]));
    if (r.length == 1U) { p->state = NR_PACKET_DELIVERED; p->delivered_at = n->time_seconds; ++n->stats.delivered; }
    else p->state = NR_PACKET_IN_FLIGHT;
    char msg[NR_EVENT_LEN]; (void)snprintf(msg, sizeof(msg), "Packet #%u generated: %d -> %d", p->id, source, destination); nr_log(n, msg);
    return (int)p->id;
}

void nr_step(NrNetwork *n, double dt) {
    if (n == NULL || !isfinite(dt) || dt <= 0.0) return;
    n->time_seconds += dt;
    for (size_t i = 0; i < n->packet_count; ++i) {
        NrPacket *p = &n->packets[i]; if (p->state != NR_PACKET_IN_FLIGHT) continue;
        if (p->ttl == 0U || p->path_index + 1U >= p->path_length) { release_packet_queue(n, p); p->state = NR_PACKET_DROPPED; ++n->stats.dropped; continue; }
        int from = p->path[p->path_index], to = p->path[p->path_index + 1U];
        NrLink *link = NULL;
        for (size_t j = 0; j < n->link_count; ++j) if (n->links[j].active && ((n->links[j].from == from && n->links[j].to == to) || (n->links[j].bidirectional && n->links[j].from == to && n->links[j].to == from))) { link = &n->links[j]; break; }
        if (link == NULL) {
            NrRoute replacement = nr_find_route(n, p->current_node, p->destination);
            if (!replacement.reachable || replacement.length < 2U) { release_packet_queue(n, p); p->state = NR_PACKET_UNREACHABLE; ++n->stats.dropped; nr_log(n, "Packet unreachable after topology change"); continue; }
            memcpy(p->path, replacement.nodes, replacement.length * sizeof(replacement.nodes[0])); p->path_length = replacement.length; p->path_index = 0U;
            from = p->current_node; to = p->path[1];
            for (size_t j = 0; j < n->link_count; ++j) if (n->links[j].active && ((n->links[j].from == from && n->links[j].to == to) || (n->links[j].bidirectional && n->links[j].from == to && n->links[j].to == from))) { link = &n->links[j]; break; }
            nr_log(n, "Packet rerouted using recalculated shortest path");
        }
        if (link == NULL) continue;
        release_packet_queue(n, p);
        const double hop_duration = fmax(0.05, link->delay_ms / 1000.0);
        p->hop_progress += dt / hop_duration;
        if (p->hop_progress < 1.0) continue;
        p->hop_progress = 0.0; ++link->packets_transmitted; --p->ttl;
        if (link->bandwidth_mbps > 0.0) link->utilization = fmin(1.0, link->utilization + 0.05);
        const double link_loss = isfinite(link->loss_probability) ? fmax(0.0, fmin(1.0, link->loss_probability)) : 1.0;
        if (((double)(nr_random(n) % 10000U) / 10000.0) < link_loss) {
            ++link->packets_lost; ++n->stats.lost; p->state = NR_PACKET_LOST; nr_log(n, "Packet lost on link (configured loss probability)"); continue;
        }
        p->current_node = to; ++p->path_index;
        NrNode *node = nr_find_node(n, to);
        if (node != NULL && (node->type == NR_ROUTER || node->type == NR_SWITCH)) {
            if (node->queue_size >= node->queue_capacity) { ++node->dropped_packets; ++n->stats.dropped; p->state = NR_PACKET_DROPPED; nr_log(n, "Packet dropped: forwarding queue full"); continue; }
            ++node->queue_size;
            p->queued_at_current = true;
        }
        if (to == p->destination) {
            p->state = NR_PACKET_DELIVERED; p->delivered_at = n->time_seconds; ++n->stats.delivered;
            const double latency = (p->delivered_at - p->created_at) * 1000.0;
            n->stats.total_latency_ms += latency; if (latency > n->stats.max_latency_ms) n->stats.max_latency_ms = latency;
            nr_log(n, "Packet delivered successfully");
        }
    }
    for (size_t i = 0; i < n->link_count; ++i) n->links[i].utilization *= fmax(0.0, 1.0 - dt * 0.15);
}

double nr_reliability(const NrNetwork *n) {
    if (n == NULL || n->stats.generated == 0U) return 100.0;
    const double delivery = (double)n->stats.delivered / (double)n->stats.generated;
    const double loss = (double)(n->stats.lost + n->stats.dropped) / (double)n->stats.generated;
    return fmax(0.0, fmin(100.0, delivery * 100.0 - loss * 25.0));
}
const char *nr_node_type_name(NrNodeType t) { static const char *names[] = {"Host", "Router", "Switch", "Server", "Wireless AP"}; return (t >= NR_HOST && t <= NR_AP) ? names[t] : "Unknown"; }
const char *nr_packet_state_name(NrPacketState s) { static const char *names[] = {"Created", "Queued", "In flight", "Delivered", "Lost", "Unreachable", "Dropped"}; return (s >= NR_PACKET_CREATED && s <= NR_PACKET_DROPPED) ? names[s] : "Unknown"; }

bool nr_load_demo(NrNetwork *n) {
    if (n == NULL) return false;
    nr_network_init(n, 424242U);
    const int h1 = nr_add_node(n, "H1", NR_HOST, 0.10F, 0.50F), r1 = nr_add_node(n, "R1", NR_ROUTER, 0.31F, 0.50F);
    const int r2 = nr_add_node(n, "R2", NR_ROUTER, 0.52F, 0.28F), r3 = nr_add_node(n, "R3", NR_ROUTER, 0.52F, 0.72F);
    const int r4 = nr_add_node(n, "R4", NR_ROUTER, 0.72F, 0.28F), h2 = nr_add_node(n, "H2", NR_HOST, 0.91F, 0.50F);
    if (h1 < 0 || r1 < 0 || r2 < 0 || r3 < 0 || r4 < 0 || h2 < 0) return false;
    (void)nr_add_link(n, h1, r1, true, 1, 10, 100); (void)nr_add_link(n, r1, r2, true, 1, 15, 100);
    (void)nr_add_link(n, r1, r3, true, 2, 18, 100); (void)nr_add_link(n, r2, r4, true, 1, 10, 100);
    (void)nr_add_link(n, r3, r4, true, 1, 12, 100); (void)nr_add_link(n, r4, h2, true, 1, 10, 100);
    nr_log(n, "Demo topology ready: six nodes, redundant paths, fixed seed 424242");
    return true;
}
