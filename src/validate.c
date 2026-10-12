#include "netrescue.h"
#include <stdio.h>
#include <string.h>

bool nr_default_endpoints(const NrNetwork *n, int *source, int *destination) {
    if (n == NULL || source == NULL || destination == NULL || n->node_count < 2U) return false;
    int first = -1, last = -1;
    for (size_t i = 0; i < n->node_count; ++i) {
        if (n->nodes[i].type != NR_HOST && n->nodes[i].type != NR_SERVER) continue;
        if (first < 0) first = n->nodes[i].id;
        last = n->nodes[i].id;
    }
    if (first < 0 || first == last) { first = n->nodes[0].id; last = n->nodes[n->node_count - 1U].id; }
    *source = first; *destination = last;
    return true;
}

static size_t add_issue(NrIssue *issues, size_t count, NrIssueSeverity severity, int node, int link, const char *message) {
    if (count >= NR_MAX_ISSUES) return count;
    issues[count].severity = severity; issues[count].node_id = node; issues[count].link_id = link;
    (void)snprintf(issues[count].message, sizeof(issues[count].message), "%s", message);
    return count + 1U;
}

/* Undirected connected components over all links (active or not), used to report disconnected graphs. */
static int count_components(const NrNetwork *n) {
    int component[NR_MAX_NODES]; int components = 0;
    for (size_t i = 0; i < NR_MAX_NODES; ++i) component[i] = -1;
    for (size_t start = 0; start < n->node_count; ++start) {
        if (component[start] >= 0) continue;
        size_t stack[NR_MAX_NODES]; size_t top = 0U;
        stack[top++] = start; component[start] = components;
        while (top > 0U) {
            const size_t u = stack[--top];
            for (size_t j = 0; j < n->link_count; ++j) {
                const NrLink *l = &n->links[j]; int other = -1;
                if (l->from == n->nodes[u].id) other = l->to; else if (l->to == n->nodes[u].id) other = l->from;
                if (other < 0) continue;
                for (size_t k = 0; k < n->node_count; ++k)
                    if (n->nodes[k].id == other && component[k] < 0) { component[k] = components; stack[top++] = k; }
            }
        }
        ++components;
    }
    return components;
}

size_t nr_validate_network(const NrNetwork *n, int source, int destination, NrIssue issues[NR_MAX_ISSUES]) {
    size_t count = 0U; char msg[NR_EVENT_LEN];
    if (n == NULL || issues == NULL) return 0U;
    if (n->node_count == 0U) return add_issue(issues, count, NR_ISSUE_ERROR, -1, -1, "The network has no nodes");
    if (n->node_count < 2U) count = add_issue(issues, count, NR_ISSUE_ERROR, -1, -1, "At least two nodes are required for a transfer");
    const NrNode *src = nr_find_node((NrNetwork *)n, source), *dst = nr_find_node((NrNetwork *)n, destination);
    if (src == NULL) count = add_issue(issues, count, NR_ISSUE_ERROR, source, -1, "Source node is not set or does not exist");
    if (dst == NULL) count = add_issue(issues, count, NR_ISSUE_ERROR, destination, -1, "Destination node is not set or does not exist");
    if (src != NULL && dst != NULL && source == destination) count = add_issue(issues, count, NR_ISSUE_ERROR, source, -1, "Source and destination must be different nodes");
    if (src != NULL && !src->active) count = add_issue(issues, count, NR_ISSUE_ERROR, source, -1, "Source node is failed (inactive)");
    if (dst != NULL && !dst->active) count = add_issue(issues, count, NR_ISSUE_ERROR, destination, -1, "Destination node is failed (inactive)");
    if (src != NULL && dst != NULL && source != destination && src->active && dst->active) {
        if (!nr_is_connected(n, source, destination))
            count = add_issue(issues, count, NR_ISSUE_ERROR, -1, -1, "Destination is unreachable from the source (disconnected network or failed elements)");
        else if (!nr_is_connected(n, destination, source))
            count = add_issue(issues, count, NR_ISSUE_ERROR, -1, -1, "No reverse path for acknowledgements (check directed links)");
    }
    const int components = count_components(n);
    if (components > 1) {
        (void)snprintf(msg, sizeof(msg), "Graph has %d disconnected components", components);
        count = add_issue(issues, count, NR_ISSUE_WARNING, -1, -1, msg);
    }
    for (size_t i = 0; i < n->node_count; ++i) {
        bool linked = false;
        for (size_t j = 0; j < n->link_count && !linked; ++j) linked = n->links[j].from == n->nodes[i].id || n->links[j].to == n->nodes[i].id;
        if (!linked) { (void)snprintf(msg, sizeof(msg), "Node %s has no links", n->nodes[i].name); count = add_issue(issues, count, NR_ISSUE_WARNING, n->nodes[i].id, -1, msg); }
        if (!n->nodes[i].active) { (void)snprintf(msg, sizeof(msg), "Node %s is failed", n->nodes[i].name); count = add_issue(issues, count, NR_ISSUE_INFO, n->nodes[i].id, -1, msg); }
    }
    for (size_t j = 0; j < n->link_count; ++j) {
        const NrLink *l = &n->links[j];
        if (l->bandwidth_mbps <= 0.0) { (void)snprintf(msg, sizeof(msg), "Link %d has zero bandwidth (modelled as unlimited)", l->id); count = add_issue(issues, count, NR_ISSUE_WARNING, -1, l->id, msg); }
        if (!l->active) { (void)snprintf(msg, sizeof(msg), "Link %d is failed", l->id); count = add_issue(issues, count, NR_ISSUE_INFO, -1, l->id, msg); }
        if (l->loss_probability > 0.0) { (void)snprintf(msg, sizeof(msg), "Link %d has a per-link loss probability of %.1f%%", l->id, l->loss_probability * 100.0); count = add_issue(issues, count, NR_ISSUE_INFO, -1, l->id, msg); }
    }
    return count;
}
