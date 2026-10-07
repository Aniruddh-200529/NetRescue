#ifndef NETRESCUE_H
#define NETRESCUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NR_MAX_NODES 64
#define NR_MAX_LINKS 256
#define NR_MAX_PACKETS 256
#define NR_MAX_HOPS NR_MAX_NODES
#define NR_NAME_LEN 32
#define NR_EVENT_LEN 128

typedef enum { NR_HOST, NR_ROUTER, NR_SWITCH, NR_SERVER, NR_AP } NrNodeType;
typedef enum { NR_PACKET_CREATED, NR_PACKET_QUEUED, NR_PACKET_IN_FLIGHT,
               NR_PACKET_DELIVERED, NR_PACKET_LOST, NR_PACKET_UNREACHABLE,
               NR_PACKET_DROPPED } NrPacketState;
typedef enum { NR_COST_LINK, NR_COST_DELAY, NR_COST_COMPOSITE } NrCostMode;

typedef struct {
    int id;
    char name[NR_NAME_LEN];
    NrNodeType type;
    float x, y;
    bool active;
    char ip[24];
    unsigned queue_capacity, queue_size;
    unsigned dropped_packets;
} NrNode;

typedef struct {
    int id, from, to;
    bool bidirectional, active;
    double cost, delay_ms, bandwidth_mbps, loss_probability;
    unsigned packets_transmitted, packets_lost;
    double utilization;
} NrLink;

typedef struct {
    unsigned id, sequence, ttl, payload_size, retransmissions;
    int source, destination, current_node;
    int path[NR_MAX_HOPS];
    size_t path_length, path_index;
    double hop_progress;
    uint32_t checksum;
    bool corrupted;
    bool queued_at_current;
    NrPacketState state;
    double created_at, delivered_at;
} NrPacket;

typedef struct {
    uint64_t generated, delivered, lost, dropped, corrupted, retransmissions;
    double total_latency_ms, max_latency_ms;
} NrStatistics;

typedef struct {
    NrNode nodes[NR_MAX_NODES];
    NrLink links[NR_MAX_LINKS];
    NrPacket packets[NR_MAX_PACKETS];
    size_t node_count, link_count, packet_count;
    unsigned next_node_id, next_link_id, next_packet_id;
    unsigned rng_state;
    double time_seconds;
    NrCostMode cost_mode;
    double weight_delay, weight_congestion;
    NrStatistics stats;
    char events[64][NR_EVENT_LEN];
    size_t event_count, event_cursor;
} NrNetwork;

typedef struct { bool reachable; size_t length; int nodes[NR_MAX_HOPS]; double cost; } NrRoute;

void nr_network_init(NrNetwork *network, unsigned seed);
int nr_add_node(NrNetwork *network, const char *name, NrNodeType type, float x, float y);
bool nr_remove_node(NrNetwork *network, int node_id);
int nr_add_link(NrNetwork *network, int from, int to, bool bidirectional,
                double cost, double delay_ms, double bandwidth_mbps);
bool nr_remove_link(NrNetwork *network, int link_id);
bool nr_set_node_active(NrNetwork *network, int node_id, bool active);
bool nr_set_link_active(NrNetwork *network, int link_id, bool active);
NrNode *nr_find_node(NrNetwork *network, int node_id);
NrLink *nr_find_link(NrNetwork *network, int link_id);
NrRoute nr_find_route(const NrNetwork *network, int source, int destination);
bool nr_is_connected(const NrNetwork *network, int source, int destination);
unsigned nr_checksum(const void *data, size_t length);
int nr_send_packet(NrNetwork *network, int source, int destination, unsigned payload_size);
void nr_step(NrNetwork *network, double delta_seconds);
void nr_log(NrNetwork *network, const char *message);
bool nr_load_demo(NrNetwork *network);
bool nr_save_topology(const NrNetwork *network, const char *path);
bool nr_load_topology(NrNetwork *network, const char *path);
double nr_reliability(const NrNetwork *network);
const char *nr_node_type_name(NrNodeType type);
const char *nr_packet_state_name(NrPacketState state);

#endif
