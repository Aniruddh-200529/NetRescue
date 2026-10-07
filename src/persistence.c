#include "netrescue.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Versioned, line-oriented format: NODE ... / LINK ...; reject partial files. */
bool nr_save_topology(const NrNetwork *n, const char *path) {
    if (n == NULL || path == NULL) return false;
    FILE *f = fopen(path, "w"); if (f == NULL) return false;
    bool ok = fprintf(f, "NETRESCUE 1\n") > 0;
    for (size_t i = 0; ok && i < n->node_count; ++i) {
        const NrNode *v = &n->nodes[i];
        ok = fprintf(f, "NODE %d %d %.6f %.6f %u %s %s\n", v->id, (int)v->type, (double)v->x, (double)v->y, v->active ? 1U : 0U, v->name, v->ip) > 0;
    }
    for (size_t i = 0; ok && i < n->link_count; ++i) {
        const NrLink *l = &n->links[i];
        ok = fprintf(f, "LINK %d %d %d %u %u %.6f %.6f %.6f %.6f\n", l->id, l->from, l->to, l->bidirectional ? 1U : 0U, l->active ? 1U : 0U, l->cost, l->delay_ms, l->bandwidth_mbps, l->loss_probability) > 0;
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool nr_load_topology(NrNetwork *n, const char *path) {
    if (n == NULL || path == NULL) return false;
    FILE *f = fopen(path, "r"); if (f == NULL) return false;
    NrNetwork candidate; nr_network_init(&candidate, 1U);
    char line[256], header[32]; unsigned version = 0U;
    bool ok = fgets(line, sizeof(line), f) != NULL && sscanf(line, "%31s %u", header, &version) == 2 && strcmp(header, "NETRESCUE") == 0 && version == 1U;
    while (ok && fgets(line, sizeof(line), f) != NULL) {
        char kind[8]; if (sscanf(line, "%7s", kind) != 1) { ok = false; break; }
        if (strcmp(kind, "NODE") == 0) {
            int id, type; double x, y; unsigned active; char name[NR_NAME_LEN], ip[24];
            if (sscanf(line, "NODE %d %d %lf %lf %u %31s %23s", &id, &type, &x, &y, &active, name, ip) != 7 || id <= 0 || type < NR_HOST || type > NR_AP || active > 1U || candidate.node_count >= NR_MAX_NODES || !isfinite(x) || !isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0 || nr_find_node(&candidate, id) != NULL) { ok = false; break; }
            int created = nr_add_node(&candidate, name, (NrNodeType)type, (float)x, (float)y); if (created < 0) { ok = false; break; }
            NrNode *node = nr_find_node(&candidate, created); node->id = id; node->active = active != 0U; (void)snprintf(node->ip, sizeof(node->ip), "%s", ip);
            if (candidate.next_node_id <= (unsigned)id) candidate.next_node_id = (unsigned)id + 1U;
        } else if (strcmp(kind, "LINK") == 0) {
            int id, from, to; unsigned bi, active; double cost, delay, bandwidth, loss;
            if (sscanf(line, "LINK %d %d %d %u %u %lf %lf %lf %lf", &id, &from, &to, &bi, &active, &cost, &delay, &bandwidth, &loss) != 9 || id <= 0 || bi > 1U || active > 1U || nr_find_link(&candidate, id) != NULL || !isfinite(loss) || loss < 0.0 || loss > 1.0) { ok = false; break; }
            int created = nr_add_link(&candidate, from, to, bi != 0U, cost, delay, bandwidth); if (created < 0) { ok = false; break; }
            NrLink *link = nr_find_link(&candidate, created); link->id = id; link->active = active != 0U; link->loss_probability = loss;
            if (candidate.next_link_id <= (unsigned)id) candidate.next_link_id = (unsigned)id + 1U;
        } else { ok = false; break; }
    }
    if (ferror(f)) ok = false;
    (void)fclose(f);
    if (ok) *n = candidate;
    return ok;
}
