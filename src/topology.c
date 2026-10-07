#include "topology.h"
#include <math.h>
#include <stdio.h>

static unsigned next_random(unsigned *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

const char *nr_topology_name(NrTopologyType type) {
    static const char *names[] = {"Line", "Bus", "Star", "Ring", "Tree", "Mesh", "Hybrid", "Random"};
    return type >= NR_TOPO_LINE && type < NR_TOPO_COUNT ? names[type] : "Unknown";
}

bool nr_generate_topology(NrNetwork *network, NrTopologyType type, size_t count, unsigned seed) {
    if (network == NULL || type < NR_TOPO_LINE || type >= NR_TOPO_COUNT || count < 2U || count > NR_MAX_NODES) return false;
    if (type == NR_TOPO_MESH && count * (count - 1U) / 2U > NR_MAX_LINKS) return false;

    nr_network_init(network, seed);
    int ids[NR_MAX_NODES];
    for (size_t i = 0; i < count; ++i) {
        float x, y;
        if (type == NR_TOPO_LINE || type == NR_TOPO_BUS) {
            x = 0.08F + 0.84F * (float)i / (float)(count - 1U); y = 0.5F + (i % 2U == 0U ? -0.04F : 0.04F);
        } else {
            const double angle = -1.5707963267948966 + 6.283185307179586 * (double)i / (double)count;
            x = (float)(0.5 + 0.37 * cos(angle)); y = (float)(0.5 + 0.37 * sin(angle));
        }
        const NrNodeType node_type = i == 0U || i + 1U == count ? NR_HOST : NR_ROUTER;
        char name[NR_NAME_LEN]; (void)snprintf(name, sizeof(name), "%s%zu", node_type == NR_HOST ? "H" : "R", i + 1U);
        ids[i] = nr_add_node(network, name, node_type, x, y);
        if (ids[i] < 0) return false;
    }

    bool connected[NR_MAX_NODES][NR_MAX_NODES] = {{false}};
    size_t made = 0U;
#define ADD_EDGE(a, b) do { \
        size_t edge_a = (a), edge_b = (b); \
        if (edge_a != edge_b && !connected[edge_a][edge_b]) { \
            if (nr_add_link(network, ids[edge_a], ids[edge_b], true, 1.0, 12.0, 100.0) < 0) return false; \
            connected[edge_a][edge_b] = true; connected[edge_b][edge_a] = true; ++made; \
        } \
    } while (0)

    switch (type) {
    case NR_TOPO_LINE:
    case NR_TOPO_BUS:
        for (size_t i = 0; i + 1U < count; ++i) ADD_EDGE(i, i + 1U);
        break;
    case NR_TOPO_STAR: {
        const size_t center = count / 2U;
        for (size_t i = 0; i < count; ++i) ADD_EDGE(center, i);
        break;
    }
    case NR_TOPO_RING:
        for (size_t i = 0; i < count; ++i) ADD_EDGE(i, (i + 1U) % count);
        break;
    case NR_TOPO_TREE:
        for (size_t i = 1; i < count; ++i) ADD_EDGE((i - 1U) / 2U, i);
        break;
    case NR_TOPO_MESH:
        for (size_t i = 0; i < count; ++i) for (size_t j = i + 1U; j < count; ++j) ADD_EDGE(i, j);
        break;
    case NR_TOPO_HYBRID:
        for (size_t i = 0; i + 1U < count; ++i) ADD_EDGE(i, i + 1U);
        for (size_t i = 0; i + 2U < count; i += 2U) ADD_EDGE(i, i + 2U);
        if (count > 3U) ADD_EDGE(0U, count - 1U);
        break;
    case NR_TOPO_RANDOM: {
        unsigned rng = seed == 0U ? 1U : seed;
        for (size_t i = 0; i + 1U < count; ++i) ADD_EDGE(i, i + 1U);
        for (size_t i = 0; i < count; ++i) for (size_t j = i + 2U; j < count; ++j) {
            if ((next_random(&rng) % 100U) < 28U) ADD_EDGE(i, j);
        }
        break;
    }
    default:
        return false;
    }
#undef ADD_EDGE
    (void)made;
    char event[NR_EVENT_LEN]; (void)snprintf(event, sizeof(event), "%s topology generated (%zu nodes, %zu links)", nr_topology_name(type), network->node_count, network->link_count);
    nr_log(network, event);
    return true;
}
