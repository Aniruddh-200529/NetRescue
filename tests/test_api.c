/* API integration tests: the same JSON requests the website sends through WebAssembly. */
#include "api.h"
#include "json.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); abort(); } } while (0)

static NrJson *last = NULL;

/* Sends a request and returns the parsed "result" (asserting ok == expect_ok). */
static const NrJson *call(const char *request, bool expect_ok) {
    nr_json_free(last);
    const char *response = nr_api_call(request);
    char error[128];
    last = nr_json_parse(response, error, sizeof(error));
    if (last == NULL) { fprintf(stderr, "unparseable response: %.300s\n", response); abort(); }
    const bool ok = nr_json_bool(nr_json_get(last, "ok"), false);
    if (ok != expect_ok) { fprintf(stderr, "request %s\nresponse %.600s\n", request, response); abort(); }
    CHECK(nr_json_number(nr_json_get(last, "api_version"), 0) == 1.0);
    return ok ? nr_json_get(last, "result") : nr_json_get(last, "error");
}

static double num(const NrJson *o, const char *key) { const NrJson *v = nr_json_get(o, key); CHECK(v != NULL); return nr_json_number(v, NAN); }

static void test_json_parser(void) {
    char error[64];
    NrJson *v = nr_json_parse("{\"a\":[1,2.5,-3e2],\"b\":{\"c\":\"x\\\"y\\u00e9\"},\"d\":true,\"e\":null}", error, sizeof(error));
    CHECK(v != NULL);
    CHECK(nr_json_get(v, "a")->count == 3U && nr_json_number(nr_json_at(nr_json_get(v, "a"), 2), 0) == -300.0);
    CHECK(strcmp(nr_json_string(nr_json_get(nr_json_get(v, "b"), "c"), ""), "x\"y\xc3\xa9") == 0);
    CHECK(nr_json_bool(nr_json_get(v, "d"), false) && !nr_json_has(v, "e"));
    nr_json_free(v);
    const char *bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "{} x", "[1e999]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) { v = nr_json_parse(bad[i], error, sizeof(error)); CHECK(v == NULL && error[0] != '\0'); }
    NrJsonWriter w; jw_init(&w);
    jw_begin_object(&w); jw_kv_number(&w, "nan", NAN); jw_kv_string(&w, "s", "a\nb\"c"); jw_key(&w, "arr"); jw_begin_array(&w); jw_int(&w, 1); jw_int(&w, 2); jw_end_array(&w); jw_end_object(&w);
    char *text = jw_take(&w);
    CHECK(strcmp(text, "{\"nan\":null,\"s\":\"a\\nb\\\"c\",\"arr\":[1,2]}") == 0);
    free(text);
}

static void test_errors(void) {
    CHECK(strstr(nr_json_string(call("not json", false), ""), "invalid request JSON") != NULL);
    CHECK(strstr(nr_json_string(call("[1]", false), ""), "object") != NULL);
    CHECK(strstr(nr_json_string(call("{}", false), ""), "cmd") != NULL);
    CHECK(strstr(nr_json_string(call("{\"cmd\":\"nope\"}", false), ""), "unknown command") != NULL);
    CHECK(strstr(nr_json_string(call("{\"cmd\":\"configure\",\"config\":{\"loss_probability\":2}}", false), ""), "loss_probability") != NULL);
    CHECK(strstr(nr_json_string(call("{\"cmd\":\"configure\",\"config\":{\"protocol\":\"tcp\"}}", false), ""), "protocol") != NULL);
    call("{\"cmd\":\"generate_topology\",\"type\":\"mesh\",\"nodes\":40}", false);
    call("{\"cmd\":\"configure\",\"scenario\":[{\"type\":\"explode\"}]}", false);
    call("{\"cmd\":\"configure\",\"scenario\":[{\"type\":\"link_outage\",\"link_id\":1,\"start\":2,\"end\":1}]}", false);
}

static void test_topology_commands(void) {
    const NrJson *r = call("{\"cmd\":\"version\"}", true);
    CHECK(strcmp(nr_json_string(nr_json_get(r, "engine"), ""), "NetRescue C simulation engine") == 0);
    r = call("{\"cmd\":\"get_network\"}", true);
    CHECK(nr_json_get(r, "nodes")->count == 6U && nr_json_get(r, "links")->count == 6U && nr_json_get(r, "route")->count == 5U);
    const char *types[] = {"line", "bus", "star", "ring", "tree", "mesh", "hybrid", "random"};
    for (size_t i = 0; i < 8U; ++i) {
        char request[128]; (void)snprintf(request, sizeof(request), "{\"cmd\":\"generate_topology\",\"type\":\"%s\",\"nodes\":9,\"seed\":3}", types[i]);
        r = call(request, true);
        CHECK(nr_json_get(r, "nodes")->count == 9U && nr_json_get(r, "route")->count >= 2U);
        for (size_t k = 0; k < nr_json_get(r, "issues")->count; ++k) CHECK(strcmp(nr_json_string(nr_json_get(nr_json_at(nr_json_get(r, "issues"), k), "severity"), ""), "error") != 0);
    }
    r = call("{\"cmd\":\"generate_topology\",\"type\":\"line\",\"nodes\":4}", true);
    CHECK(num(r, "source") == 1.0 && num(r, "destination") == 4.0);
    r = call("{\"cmd\":\"add_node\",\"type\":\"server\",\"name\":\"Web Server\",\"x\":0.5,\"y\":0.9}", true);
    CHECK(nr_json_get(r, "nodes")->count == 5U);
    const NrJson *added = nr_json_at(nr_json_get(r, "nodes"), 4);
    CHECK(strcmp(nr_json_string(nr_json_get(added, "name"), ""), "Web_Server") == 0 && strcmp(nr_json_string(nr_json_get(added, "type"), ""), "server") == 0);
    /* The isolated server makes the graph disconnected, which validation reports. */
    bool warned = false;
    for (size_t k = 0; k < nr_json_get(r, "issues")->count; ++k) if (strstr(nr_json_string(nr_json_get(nr_json_at(nr_json_get(r, "issues"), k), "message"), ""), "no links") != NULL) warned = true;
    CHECK(warned);
    call("{\"cmd\":\"set_endpoints\",\"source\":1,\"destination\":5}", true);
    r = call("{\"cmd\":\"get_network\"}", true);
    CHECK(nr_json_get(r, "route")->count == 0U);
    call("{\"cmd\":\"sim_start\"}", false); /* unreachable destination is refused */
    r = call("{\"cmd\":\"add_link\",\"from\":4,\"to\":5,\"delay_ms\":3,\"bandwidth_mbps\":50,\"jitter_ms\":1}", true);
    CHECK(nr_json_get(r, "route")->count == 5U);
    call("{\"cmd\":\"add_link\",\"from\":4,\"to\":5}", false); /* duplicate */
    r = call("{\"cmd\":\"update_link\",\"id\":2,\"bandwidth_mbps\":2.5,\"loss_probability\":0.1}", true);
    CHECK(fabs(num(nr_json_at(nr_json_get(r, "links"), 1), "bandwidth_mbps") - 2.5) < 1e-9);
    call("{\"cmd\":\"update_link\",\"id\":2,\"loss_probability\":7}", false);
    r = call("{\"cmd\":\"update_node\",\"id\":2,\"x\":0.25,\"queue_capacity\":4,\"type\":\"switch\"}", true);
    CHECK(num(nr_json_at(nr_json_get(r, "nodes"), 1), "queue_capacity") == 4.0);
    /* Save / load round trip through the API. */
    r = call("{\"cmd\":\"export_topology\"}", true);
    const char *text = nr_json_string(nr_json_get(r, "text"), "");
    CHECK(strncmp(text, "NETRESCUE 2", 11) == 0);
    NrJsonWriter w; jw_init(&w); jw_begin_object(&w); jw_kv_string(&w, "cmd", "import_topology"); jw_kv_string(&w, "text", text); jw_end_object(&w);
    char *request = jw_take(&w);
    call("{\"cmd\":\"load_demo\"}", true);
    r = call(request, true);
    free(request);
    CHECK(nr_json_get(r, "nodes")->count == 5U && nr_json_get(r, "links")->count == 4U);
    call("{\"cmd\":\"import_topology\",\"text\":\"garbage\"}", false);
    r = call("{\"cmd\":\"remove_node\",\"id\":5}", true);
    CHECK(nr_json_get(r, "nodes")->count == 4U && nr_json_get(r, "links")->count == 3U);
}

static void test_live_simulation(void) {
    call("{\"cmd\":\"generate_topology\",\"type\":\"line\",\"nodes\":4}", true);
    call("{\"cmd\":\"set_all_links\",\"delay_ms\":10,\"bandwidth_mbps\":10}", true);
    const NrJson *r = call("{\"cmd\":\"sim_start\",\"config\":{\"protocol\":\"selective_repeat\",\"packet_count\":12,\"window_size\":4,"
                           "\"payload_bytes\":1000,\"packets_per_second\":1000,\"timeout_s\":0.15,\"max_retries\":15},"
                           "\"scenario\":[{\"type\":\"drop_data\",\"sequence\":3,\"attempt\":1}]}", true);
    CHECK(strcmp(nr_json_string(nr_json_get(r, "state"), ""), "Running") == 0);
    call("{\"cmd\":\"remove_node\",\"id\":2}", false); /* structure is locked while running */
    call("{\"cmd\":\"set_endpoints\",\"source\":1,\"destination\":3}", false);
    r = call("{\"cmd\":\"sim_step\",\"dt\":0.02}", true);
    CHECK(num(r, "time") > 0.019 && nr_json_get(r, "in_flight")->count > 0U);
    const NrJson *frame = nr_json_at(nr_json_get(r, "in_flight"), 0);
    CHECK(strcmp(nr_json_string(nr_json_get(frame, "kind"), ""), "data") == 0 && num(frame, "progress") >= 0.0 && num(frame, "progress") <= 1.0);
    CHECK(strlen(nr_json_string(nr_json_get(r, "sender_map"), "")) == 12U);
    call("{\"cmd\":\"sim_pause\"}", true);
    const double paused_at = num(call("{\"cmd\":\"sim_step\",\"dt\":1}", true), "time");
    r = call("{\"cmd\":\"sim_step_event\"}", true);
    CHECK(num(r, "time") >= paused_at && strcmp(nr_json_string(nr_json_get(r, "state"), ""), "Paused") == 0);
    call("{\"cmd\":\"sim_resume\"}", true);
    /* Link property edits are allowed while running. */
    call("{\"cmd\":\"update_link\",\"id\":3,\"delay_ms\":12}", true);
    for (int i = 0; i < 200; ++i) { r = call("{\"cmd\":\"sim_step\",\"dt\":0.05,\"trace_from\":0,\"trace_limit\":5000}", true); if (strcmp(nr_json_string(nr_json_get(r, "state"), ""), "Running") != 0) break; }
    CHECK(strcmp(nr_json_string(nr_json_get(r, "state"), ""), "Complete") == 0);
    const NrJson *m = nr_json_get(r, "metrics");
    CHECK(num(m, "packets_delivered") == 12.0 && num(m, "retransmissions") == 1.0 && num(m, "delivery_ratio") == 1.0);
    CHECK(num(nr_json_get(nr_json_get(m, "faults"), "data_loss"), "injected") == 1.0);
    /* The returned trace agrees with the metrics. */
    const NrJson *trace = nr_json_get(r, "trace");
    CHECK(trace->count == (size_t)num(r, "trace_total"));
    size_t sends = 0U, retx = 0U, lost = 0U, delivered = 0U;
    for (size_t i = 0; i < trace->count; ++i) {
        const char *type = nr_json_string(nr_json_get(nr_json_at(trace, i), "type"), "");
        if (strcmp(type, "DATA_SEND") == 0) ++sends;
        if (strcmp(type, "DATA_RETX") == 0) { ++retx; CHECK(num(nr_json_at(trace, i), "seq") == 3.0); }
        if (strcmp(type, "DATA_LOST") == 0) ++lost;
        if (strcmp(type, "DATA_DELIVERED") == 0) ++delivered;
    }
    CHECK(sends == 12U && retx == 1U && lost == 1U && delivered == 12U);
    const NrJson *faults = nr_json_get(r, "faults");
    CHECK(faults != NULL && faults->count == 1U);
    CHECK(nr_json_bool(nr_json_get(nr_json_at(faults, 0), "recovered"), false));
    /* Faults are omitted when the caller already has the current version. */
    char request[128]; (void)snprintf(request, sizeof(request), "{\"cmd\":\"sim_state\",\"fault_version\":%g,\"history_version\":%g}", num(r, "fault_version"), num(r, "history_version"));
    r = call(request, true);
    CHECK(nr_json_get(r, "faults") == NULL && nr_json_get(r, "history") == NULL);
    r = call("{\"cmd\":\"sim_export\"}", true);
    CHECK(strncmp(nr_json_string(nr_json_get(r, "trace_csv"), ""), "index,time_s,type", 17) == 0);
    /* Finished simulations do not block editing. */
    call("{\"cmd\":\"update_node\",\"id\":2,\"name\":\"Core\"}", true);
    r = call("{\"cmd\":\"sim_reset\"}", true);
    CHECK(strcmp(nr_json_string(nr_json_get(r, "state"), ""), "Idle") == 0);
}

static void test_experiments(void) {
    const NrJson *r = call("{\"cmd\":\"list_presets\"}", true);
    CHECK(r->count == 7U);
    r = call("{\"cmd\":\"preset_spec\",\"id\":\"loss_sweep\"}", true);
    CHECK(strcmp(nr_json_string(nr_json_get(nr_json_get(nr_json_get(r, "spec"), "sweep"), "parameter"), ""), "loss_probability") == 0);
    r = call("{\"cmd\":\"run_preset\",\"id\":\"gbn_vs_sr\"}", true);
    const NrJson *runs = nr_json_get(r, "runs");
    CHECK(runs->count == 2U && nr_json_get(r, "captures")->count == 2U && nr_json_get(r, "findings")->count > 0U);
    CHECK(nr_json_get(nr_json_at(runs, 0), "retransmitted")->count == 8U && nr_json_get(nr_json_at(runs, 1), "retransmitted")->count == 1U);
    r = call("{\"cmd\":\"export_experiment_csv\"}", true);
    CHECK(strncmp(nr_json_string(nr_json_get(r, "csv"), ""), "sweep_parameter,", 16) == 0);
    /* A custom comparison on a generated topology with repeated trials. */
    r = call("{\"cmd\":\"run_experiment\",\"topology\":{\"kind\":\"generate\",\"type\":\"ring\",\"nodes\":6,\"seed\":2},"
             "\"config\":{\"packet_count\":40,\"window_size\":6,\"loss_probability\":0.1,\"timeout_s\":0.2},"
             "\"protocols\":[\"go_back_n\",\"selective_repeat\"],\"trials\":4,\"seed_base\":5,\"seed_stride\":3,"
             "\"sweep\":{\"parameter\":\"ack_loss_probability\",\"values\":[0,0.2]},\"overrides\":{\"link_delay_ms\":4}}", true);
    CHECK(nr_json_get(r, "runs")->count == 16U && nr_json_get(r, "aggregates")->count == 4U);
    const NrJson *agg = nr_json_at(nr_json_get(r, "aggregates"), 0);
    CHECK(num(nr_json_get(nr_json_get(agg, "stats"), "goodput_bps"), "n") == 4.0);
    CHECK(num(nr_json_at(nr_json_get(r, "runs"), 1), "seed") == 8.0);
    /* Identical requests give identical results. */
    char *first = NULL;
    for (int k = 0; k < 2; ++k) {
        const char *response = nr_api_call("{\"cmd\":\"run_experiment\",\"topology\":{\"kind\":\"preset\",\"id\":\"ack_loss\"},\"trials\":2}");
        const char *runs_at = strstr(response, "\"runs\"");
        CHECK(runs_at != NULL);
        if (k == 0) { first = malloc(strlen(runs_at) + 1U); strcpy(first, runs_at); }
        else CHECK(strcmp(first, runs_at) == 0);
    }
    free(first);
    call("{\"cmd\":\"run_experiment\",\"protocols\":[],\"trials\":1}", false);
    call("{\"cmd\":\"run_experiment\",\"trials\":0}", false);
    call("{\"cmd\":\"run_experiment\",\"sweep\":{\"parameter\":\"loss_probability\",\"values\":[]}}", false);
    r = call("{\"cmd\":\"metric_definitions\"}", true);
    CHECK(r->count >= 10U && strlen(nr_json_string(nr_json_get(nr_json_at(r, 0), "formula"), "")) > 0U);
}

int main(void) {
    test_json_parser();
    test_errors();
    test_topology_commands();
    test_live_simulation();
    test_experiments();
    nr_json_free(last);
    nr_api_shutdown();
    printf("All NetRescue API tests passed (%d checks).\n", checks);
    return 0;
}
