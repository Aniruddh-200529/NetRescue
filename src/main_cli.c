#include "netrescue.h"
#include "cidr.h"
#include "experiment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Original deterministic packet-routing demonstration (unchanged behaviour when run without arguments). */
static int routing_demo(void) {
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

static int run_preset(const char *id) {
    const int index = nr_preset_find(id);
    if (index < 0) { fprintf(stderr, "unknown preset '%s' (use --list-presets)\n", id); return 2; }
    NrNetwork *network = malloc(sizeof(*network));
    NrExperimentResult *result = malloc(sizeof(*result));
    NrExperimentSpec spec;
    if (network == NULL || result == NULL || !nr_preset_build((size_t)index, network, &spec)) { free(network); free(result); return 1; }
    const NrPresetInfo *info = nr_preset_info((size_t)index);
    printf("%s\n%s\nSettings: %s\n\n", info->title, info->purpose, info->settings);
    if (!nr_experiment_run(network, &spec, result)) { fprintf(stderr, "experiment failed: %s\n", result->error); free(network); free(result); return 1; }
    printf("%-10s %-17s %9s %8s %12s %10s %10s %8s %8s\n", "value", "protocol", "completed", "PDR", "goodput", "latency", "time", "retx", "unneeded");
    for (size_t i = 0; i < result->aggregate_count; ++i) {
        const NrExperimentAggregate *a = &result->aggregates[i];
        char value[16] = "-";
        if (spec.sweep != NR_SWEEP_NONE) (void)snprintf(value, sizeof(value), "%g", a->sweep_value);
        printf("%-10s %-17s %5u/%-3u %7.1f%% %9.1f kb/s %7.1f ms %8.3f s %8.1f %8.1f\n", value, nr_arq_protocol_name(a->protocol), a->completed, a->runs,
               a->stats[NR_AGG_DELIVERY_RATIO].mean * 100.0, a->stats[NR_AGG_GOODPUT].mean / 1000.0, a->stats[NR_AGG_LATENCY_AVG].mean,
               a->stats[NR_AGG_COMPLETION_TIME].mean, a->stats[NR_AGG_RETRANSMISSIONS].mean, a->stats[NR_AGG_RETX_UNNECESSARY].mean);
    }
    puts("\nFindings (generated from the measured results):");
    for (size_t i = 0; i < result->finding_count; ++i) printf(" - %s\n", result->findings[i]);
    nr_experiment_free(result); free(result); free(network);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list-presets") == 0) {
        for (size_t i = 0; i < nr_preset_count(); ++i) printf("%-12s %s\n", nr_preset_info(i)->id, nr_preset_info(i)->title);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--preset") == 0) return run_preset(argv[2]);
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        puts("usage: NetRescueCLI                 routing demonstration\n"
             "       NetRescueCLI --list-presets  list the prebuilt experiments\n"
             "       NetRescueCLI --preset <id>   run a prebuilt experiment and print its results");
        return 0;
    }
    return routing_demo();
}
