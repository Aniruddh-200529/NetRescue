#include "netrescue.h"
#include "cidr.h"
#include <stdio.h>

int main(void) {
    NrNetwork network;
    if (!nr_load_demo(&network)) return 1;
    const int source = network.nodes[0].id, destination = network.nodes[5].id;
    (void)nr_send_packet(&network, source, destination, 24U);
    for (unsigned i = 0; i < 100U && network.stats.delivered == 0U; ++i) nr_step(&network, 0.02);
    printf("NetRescue C11 simulator | nodes=%zu links=%zu | delivered=%llu | reliability=%.1f/100\n", network.node_count, network.link_count, (unsigned long long)network.stats.delivered, nr_reliability(&network));
    NrRoute route = nr_find_route(&network, source, destination);
    printf("Shortest path:");
    for (size_t i = 0; i < route.length; ++i) { NrNode *node = nr_find_node(&network, route.nodes[i]); printf(" %s%s", i == 0U ? "" : "-> ", node == NULL ? "?" : node->name); }
    puts("");
    return network.stats.delivered == 1U ? 0 : 1;
}
