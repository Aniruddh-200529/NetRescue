#ifndef NETRESCUE_TOPOLOGY_H
#define NETRESCUE_TOPOLOGY_H
#include "netrescue.h"

typedef enum {
    NR_TOPO_LINE,
    NR_TOPO_BUS,
    NR_TOPO_STAR,
    NR_TOPO_RING,
    NR_TOPO_TREE,
    NR_TOPO_MESH,
    NR_TOPO_HYBRID,
    NR_TOPO_RANDOM,
    NR_TOPO_COUNT
} NrTopologyType;

bool nr_generate_topology(NrNetwork *network, NrTopologyType type, size_t node_count, unsigned seed);
const char *nr_topology_name(NrTopologyType type);

#endif
