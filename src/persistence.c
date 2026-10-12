#include "netrescue.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Versioned, line-oriented topology format. Partial or invalid files are rejected and the
 * caller's network is only replaced on success.
 *
 *   NETRESCUE 2
 *   NODE <id> <type> <x> <y> <active> <name> <ip> <queue_capacity>
 *   LINK <id> <from> <to> <bidirectional> <active> <cost> <delay_ms> <bandwidth_mbps> <loss> <jitter_ms>
 *
 * Version 1 files (without queue_capacity and jitter_ms) are still accepted.
 */

typedef struct { char *data; size_t length, capacity; bool ok; } TextBuffer;

static void text_append(TextBuffer *b, const char *format, ...) {
    if (!b->ok) return;
    for (;;) {
        va_list args; va_start(args, format);
        const int written = vsnprintf(b->data + b->length, b->capacity - b->length, format, args);
        va_end(args);
        if (written < 0) { b->ok = false; return; }
        if ((size_t)written < b->capacity - b->length) { b->length += (size_t)written; return; }
        const size_t capacity = b->capacity * 2U + (size_t)written;
        char *grown = realloc(b->data, capacity);
        if (grown == NULL) { b->ok = false; return; }
        b->data = grown; b->capacity = capacity;
    }
}

char *nr_topology_to_text(const NrNetwork *n) {
    if (n == NULL) return NULL;
    TextBuffer b = {malloc(4096), 0U, 4096U, true};
    if (b.data == NULL) return NULL;
    b.data[0] = '\0';
    text_append(&b, "NETRESCUE 2\n");
    for (size_t i = 0; i < n->node_count; ++i) {
        const NrNode *v = &n->nodes[i];
        text_append(&b, "NODE %d %d %.6f %.6f %u %s %s %u\n", v->id, (int)v->type, (double)v->x, (double)v->y,
                    v->active ? 1U : 0U, v->name, v->ip, v->queue_capacity);
    }
    for (size_t i = 0; i < n->link_count; ++i) {
        const NrLink *l = &n->links[i];
        text_append(&b, "LINK %d %d %d %u %u %.6f %.6f %.6f %.6f %.6f\n", l->id, l->from, l->to, l->bidirectional ? 1U : 0U,
                    l->active ? 1U : 0U, l->cost, l->delay_ms, l->bandwidth_mbps, l->loss_probability, l->jitter_ms);
    }
    if (!b.ok) { free(b.data); return NULL; }
    return b.data;
}

static bool parse_line(NrNetwork *candidate, const char *line, unsigned version) {
    char kind[8];
    if (sscanf(line, "%7s", kind) != 1) return false;
    if (strcmp(kind, "NODE") == 0) {
        int id, type; double x, y; unsigned active, queue = 10U; char name[NR_NAME_LEN], ip[24];
        const int expected = version >= 2U ? 8 : 7;
        if (sscanf(line, "NODE %d %d %lf %lf %u %31s %23s %u", &id, &type, &x, &y, &active, name, ip, &queue) != expected) return false;
        if (id <= 0 || type < NR_HOST || type > NR_AP || active > 1U || candidate->node_count >= NR_MAX_NODES ||
            !isfinite(x) || !isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0 || nr_find_node(candidate, id) != NULL ||
            queue == 0U || queue > 4096U) return false;
        const int created = nr_add_node(candidate, name, (NrNodeType)type, (float)x, (float)y);
        if (created < 0) return false;
        NrNode *node = nr_find_node(candidate, created);
        node->id = id; node->active = active != 0U; node->queue_capacity = queue;
        (void)snprintf(node->ip, sizeof(node->ip), "%s", ip);
        if (candidate->next_node_id <= (unsigned)id) candidate->next_node_id = (unsigned)id + 1U;
        return true;
    }
    if (strcmp(kind, "LINK") == 0) {
        int id, from, to; unsigned bi, active; double cost, delay, bandwidth, loss, jitter = 0.0;
        const int expected = version >= 2U ? 10 : 9;
        if (sscanf(line, "LINK %d %d %d %u %u %lf %lf %lf %lf %lf", &id, &from, &to, &bi, &active, &cost, &delay, &bandwidth, &loss, &jitter) != expected) return false;
        if (id <= 0 || bi > 1U || active > 1U || nr_find_link(candidate, id) != NULL || !isfinite(loss) || loss < 0.0 || loss > 1.0 ||
            !isfinite(jitter) || jitter < 0.0) return false;
        const int created = nr_add_link(candidate, from, to, bi != 0U, cost, delay, bandwidth);
        if (created < 0) return false;
        NrLink *link = nr_find_link(candidate, created);
        link->id = id; link->active = active != 0U; link->loss_probability = loss; link->jitter_ms = jitter;
        if (candidate->next_link_id <= (unsigned)id) candidate->next_link_id = (unsigned)id + 1U;
        return true;
    }
    return false;
}

bool nr_topology_from_text(NrNetwork *n, const char *text) {
    if (n == NULL || text == NULL) return false;
    NrNetwork *candidate = malloc(sizeof(*candidate));
    if (candidate == NULL) return false;
    nr_network_init(candidate, 1U);
    char line[256], header[32]; unsigned version = 0U; bool first = true, ok = true;
    const char *cursor = text;
    while (ok && *cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
        if (length >= sizeof(line)) { ok = false; break; }
        memcpy(line, cursor, length); line[length] = '\0';
        if (length > 0U && line[length - 1U] == '\r') line[--length] = '\0';
        cursor = end == NULL ? cursor + length : end + 1;
        if (first) {
            first = false;
            ok = sscanf(line, "%31s %u", header, &version) == 2 && strcmp(header, "NETRESCUE") == 0 && (version == 1U || version == 2U);
            continue;
        }
        if (length == 0U) continue;
        ok = parse_line(candidate, line, version);
    }
    if (first) ok = false;
    if (ok) {
        char message[NR_EVENT_LEN];
        (void)snprintf(message, sizeof(message), "Topology loaded (%zu nodes, %zu links)", candidate->node_count, candidate->link_count);
        nr_log(candidate, message);
        *n = *candidate;
    }
    free(candidate);
    return ok;
}

bool nr_save_topology(const NrNetwork *n, const char *path) {
    if (n == NULL || path == NULL) return false;
    char *text = nr_topology_to_text(n);
    if (text == NULL) return false;
    FILE *f = fopen(path, "w");
    if (f == NULL) { free(text); return false; }
    bool ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = false;
    free(text);
    return ok;
}

bool nr_load_topology(NrNetwork *n, const char *path) {
    if (n == NULL || path == NULL) return false;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    size_t capacity = 4096U, length = 0U; char *text = malloc(capacity);
    bool ok = text != NULL;
    while (ok) {
        if (length + 1024U >= capacity) {
            char *grown = realloc(text, capacity * 2U);
            if (grown == NULL) { ok = false; break; }
            text = grown; capacity *= 2U;
        }
        const size_t got = fread(text + length, 1U, capacity - length - 1U, f);
        length += got;
        if (got == 0U) break;
    }
    if (ferror(f)) ok = false;
    (void)fclose(f);
    if (ok) { text[length] = '\0'; ok = nr_topology_from_text(n, text); }
    free(text);
    return ok;
}
