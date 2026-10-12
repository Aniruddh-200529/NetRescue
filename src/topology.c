#include "topology.h"
#include <math.h>
#include <stdio.h>

static unsigned next_random(unsigned *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

/* Device roles: transfer endpoints are hosts; forwarding devices are routers or switches. */
static NrNodeType node_role(NrTopologyType type, size_t i, size_t count) {
    switch (type) {
    case NR_TOPO_STAR: return i == count / 2U ? NR_SWITCH : NR_HOST;
    case NR_TOPO_RING: return i == 0U || i == count / 2U ? NR_HOST : NR_ROUTER;
    case NR_TOPO_TREE:
        if (i == 0U) return count == 1U ? NR_HOST : NR_ROUTER;
        return 2U * i + 1U >= count ? NR_HOST : NR_SWITCH;
    default: return i == 0U || i + 1U == count ? NR_HOST : NR_ROUTER;
    }
}

static void node_position(NrTopologyType type, size_t i, size_t count, float *x, float *y) {
    if (type == NR_TOPO_LINE || type == NR_TOPO_BUS) {
        *x = 0.08F + 0.84F * (float)i / (float)(count - 1U); *y = 0.5F + (i % 2U == 0U ? -0.04F : 0.04F);
        return;
    }
    if (type == NR_TOPO_TREE) {
        size_t level = 0U, first = 0U, width = 1U;
        while (i >= first + width) { first += width; width *= 2U; ++level; }
        size_t levels = 0U, f = 0U, w = 1U;
        while (f < count) { f += w; w *= 2U; ++levels; }
        *x = (float)(0.06 + 0.88 * ((double)(i - first) + 0.5) / (double)width);
        *y = (float)(levels <= 1U ? 0.5 : 0.1 + 0.8 * (double)level / (double)(levels - 1U));
        return;
    }
    size_t slot = i, slots = count;
    if (type == NR_TOPO_STAR) {
        if (i == count / 2U) { *x = 0.5F; *y = 0.5F; return; }
        slot = i < count / 2U ? i : i - 1U; slots = count - 1U;
    }
    const double angle = -1.5707963267948966 + 6.283185307179586 * (double)slot / (double)slots;
    *x = (float)(0.5 + 0.4 * cos(angle)); *y = (float)(0.5 + 0.4 * sin(angle));
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
        node_position(type, i, count, &x, &y);
        const NrNodeType node_type = node_role(type, i, count);
        static const char *prefix[] = {"H", "R", "S", "SV", "AP"};
        char name[NR_NAME_LEN]; (void)snprintf(name, sizeof(name), "%s%zu", prefix[node_type], i + 1U);
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
