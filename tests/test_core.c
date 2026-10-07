#include "netrescue.h"
#include "cidr.h"
#include "arq.h"
#include "topology.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_graph_and_routing(void) {
    NrNetwork n; nr_network_init(&n, 7U);
    int a = nr_add_node(&n, "A", NR_HOST, 0, 0), b = nr_add_node(&n, "B", NR_ROUTER, 1, 0), c = nr_add_node(&n, "C", NR_HOST, 2, 0);
    assert(a > 0 && b > 0 && c > 0 && nr_add_node(&n, "bad", NR_HOST, 0, 0) > 0);
    assert(nr_add_link(&n, a, a, true, 1, 1, 1) < 0);
    int ab = nr_add_link(&n, a, b, true, 1, 1, 10), bc = nr_add_link(&n, b, c, true, 1, 1, 10);
    assert(ab > 0 && bc > 0 && nr_add_link(&n, a, b, true, 1, 1, 10) < 0);
    NrRoute r = nr_find_route(&n, a, c); assert(r.reachable && r.length == 3U && r.cost == 2.0);
    assert(nr_set_link_active(&n, bc, false)); assert(!nr_is_connected(&n, a, c));
    assert(nr_set_node_active(&n, b, false)); assert(!nr_is_connected(&n, a, c));
    assert(nr_set_node_active(&n, b, true) && !nr_is_connected(&n, a, c));
    assert(nr_set_link_active(&n, bc, true) && nr_is_connected(&n, a, c));
    assert(nr_remove_node(&n, b)); assert(n.link_count == 0U);
    assert(nr_add_node(&n, "nan", NR_HOST, NAN, 0.0F) < 0);
    assert(nr_add_node(&n, "inf", NR_HOST, INFINITY, 0.0F) < 0);
}
static void test_packets_and_recovery(void) {
    NrNetwork n; assert(nr_load_demo(&n)); int source = n.nodes[0].id, dest = n.nodes[5].id;
    int packet = nr_send_packet(&n, source, dest, 8U); assert(packet > 0);
    for (unsigned i = 0; i < 300U && n.stats.delivered == 0U; ++i) nr_step(&n, 0.02);
    assert(n.stats.delivered == 1U && n.packets[0].state == NR_PACKET_DELIVERED);
    assert(nr_set_link_active(&n, 4, false));
    packet = nr_send_packet(&n, source, dest, 8U); assert(packet > 0);
    assert(n.packets[1].path_length >= 4U);
    for (unsigned i = 0; i < 300U && n.stats.delivered < 2U; ++i) nr_step(&n, 0.02);
    assert(n.stats.delivered == 2U);
    assert(nr_checksum("abc", 3U) == nr_checksum("abc", 3U)); assert(nr_checksum("abc", 3U) != nr_checksum("abd", 3U));
}
static void test_cidr(void) {
    NrCidr c; char out[16];
    assert(nr_parse_cidr("192.168.10.25/26", &c) && c.usable_hosts == 62U);
    nr_format_ipv4(c.network, out); assert(strcmp(out, "192.168.10.0") == 0);
    assert(nr_parse_cidr("1.2.3.4/0", &c) && c.usable_hosts == 4294967294ULL);
    assert(nr_parse_cidr("10.0.0.1/32", &c) && c.usable_hosts == 1U);
    assert(nr_parse_cidr("10.0.0.0/31", &c) && c.usable_hosts == 2U);
    assert(!nr_parse_cidr("256.1.1.1/24", &c)); assert(!nr_parse_cidr("1.2.3.4/33", &c)); assert(!nr_parse_cidr("1.2.3.4/no", &c));
}
static void test_persistence(void) {
    NrNetwork n, restored; assert(nr_load_demo(&n));
    assert(nr_save_topology(&n, "netrescue_test.topo")); nr_network_init(&restored, 5U);
    assert(nr_load_topology(&restored, "netrescue_test.topo")); assert(restored.node_count == n.node_count && restored.link_count == n.link_count);
    (void)remove("netrescue_test.topo");
}
static void test_empty_and_directional_graphs(void) {
    NrNetwork n; nr_network_init(&n, 3U);
    assert(!nr_find_route(&n, 1, 1).reachable);
    int a = nr_add_node(&n, "A", NR_HOST, 0, 0);
    assert(a > 0 && nr_find_route(&n, a, a).reachable);
    int packet = nr_send_packet(&n, a, a, 0U);
    assert(packet > 0 && n.packets[0].state == NR_PACKET_DELIVERED && n.stats.delivered == 1U);
    int b = nr_add_node(&n, "B", NR_HOST, 1, 0);
    int edge = nr_add_link(&n, a, b, false, 1.0, 5.0, 10.0);
    assert(edge > 0 && nr_is_connected(&n, a, b) && !nr_is_connected(&n, b, a));
    assert(nr_add_link(&n, a, b, true, NAN, 1.0, 1.0) < 0);
    assert(nr_add_link(&n, a, b, true, 1.0, 1.0, INFINITY) < 0);
    assert(nr_set_node_active(&n, b, false) && !nr_is_connected(&n, a, b));
    assert(!nr_set_link_active(&n, 999, true) && !nr_remove_link(&n, 999));
    assert(!nr_remove_node(&n, 999));
}
static void test_loss_ttl_and_queue_boundaries(void) {
    NrNetwork n; nr_network_init(&n, 11U);
    int a = nr_add_node(&n, "A", NR_HOST, 0, 0), r = nr_add_node(&n, "R", NR_ROUTER, .5F, .5F), b = nr_add_node(&n, "B", NR_HOST, 1, 1);
    int ar = nr_add_link(&n, a, r, true, 1, 1, 10), rb = nr_add_link(&n, r, b, true, 1, 1, 10);
    assert(ar > 0 && rb > 0);
    NrLink *lossy = nr_find_link(&n, ar); assert(lossy != NULL); lossy->loss_probability = 1.0;
    assert(nr_send_packet(&n, a, b, 32U) > 0); nr_step(&n, 1.0);
    assert(n.packets[0].state == NR_PACKET_LOST && n.stats.lost == 1U && lossy->packets_lost == 1U);
    lossy->loss_probability = 0.0;
    assert(nr_send_packet(&n, a, b, 32U) > 0); n.packets[1].ttl = 1U;
    nr_step(&n, 1.0); nr_step(&n, 1.0);
    NrNode *router = nr_find_node(&n, r); assert(router != NULL); router->queue_capacity = 1U;
    assert(n.packets[1].state == NR_PACKET_DROPPED && n.stats.dropped == 1U && router->queue_size == 0U);
    assert(nr_send_packet(&n, a, b, 1U) > 0 && nr_send_packet(&n, a, b, 1U) > 0);
    nr_step(&n, 1.0);
    assert(router->queue_size == 1U && router->dropped_packets == 1U);
}
static void test_node_capacity_and_persistence_errors(void) {
    NrNetwork n, target; nr_network_init(&n, 1U);
    for (int i = 0; i < NR_MAX_NODES; ++i) { char name[NR_NAME_LEN]; (void)snprintf(name, sizeof(name), "N%d", i); assert(nr_add_node(&n, name, NR_HOST, 0, 0) > 0); }
    assert(n.node_count == NR_MAX_NODES && nr_add_node(&n, "overflow", NR_HOST, 0, 0) < 0);
    nr_network_init(&target, 9U); int retained = nr_add_node(&target, "kept", NR_HOST, 0, 0); assert(retained > 0);
    FILE *f = fopen("netrescue_invalid.topo", "w"); assert(f != NULL); fputs("", f); assert(fclose(f) == 0);
    assert(!nr_load_topology(&target, "netrescue_invalid.topo") && target.node_count == 1U && target.nodes[0].id == retained);
    assert(!nr_load_topology(&target, "netrescue_missing.topo"));
    (void)remove("netrescue_invalid.topo");
}
static void test_nan_inputs_are_ignored(void) {
    NrNetwork n; nr_network_init(&n, 1U); int a=nr_add_node(&n,"A",NR_HOST,0,0),b=nr_add_node(&n,"B",NR_HOST,1,1);
    assert(nr_add_link(&n,a,b,true,1,1,1)>0); assert(nr_send_packet(&n,a,b,10U)>0);
    double before=n.time_seconds; nr_step(&n,NAN); assert(n.time_seconds==before);
    n.links[0].loss_probability=NAN; nr_step(&n,0.2); assert(n.packets[0].state==NR_PACKET_LOST);
}
static void test_cidr_prefix_boundaries(void) {
    NrCidr c;
    assert(nr_parse_cidr("10.1.2.3/8", &c) && c.usable_hosts == 16777214U);
    assert(nr_parse_cidr("172.16.2.3/16", &c) && c.usable_hosts == 65534U);
    assert(nr_parse_cidr("192.168.1.2/24", &c) && c.usable_hosts == 254U);
    assert(nr_parse_cidr("192.0.2.5/30", &c) && c.usable_hosts == 2U);
    assert(nr_parse_cidr("1.2.3.4/0", &c) && c.mask == 0U && c.network == 0U && c.broadcast == UINT32_MAX);
    assert(nr_checksum(NULL, 0U) == 2166136261U && nr_checksum(NULL, 1U) == 0U);
}
static NrArqConfig arq_config(NrArqProtocol protocol, size_t packets, unsigned window) {
    NrArqConfig c; memset(&c, 0, sizeof(c)); c.protocol = protocol; c.packet_count = packets; c.window_size = window;
    c.payload_bytes = 128U; c.packets_per_second = 100.0; c.timeout_seconds = 0.12; c.seed = 42U; return c;
}
static void run_arq(NrArqSimulation *s) {
    for (unsigned i = 0; i < 20000U && s->state == NR_ARQ_RUNNING; ++i) nr_arq_step(s, 0.01);
}
static void test_topology_templates(void) {
    NrNetwork n;
    for (int type = 0; type < NR_TOPO_COUNT; ++type) {
        assert(nr_generate_topology(&n, (NrTopologyType)type, 8U, 77U));
        assert(n.node_count == 8U && n.link_count >= 7U && nr_is_connected(&n, n.nodes[0].id, n.nodes[7].id));
    }
    /* A two-node ring is represented as one undirected edge in this simple graph. */
    assert(nr_generate_topology(&n, NR_TOPO_RING, 2U, 1U) && n.link_count == 1U);
    assert(!nr_generate_topology(&n, NR_TOPO_MESH, 24U, 1U));
    assert(!nr_generate_topology(&n, NR_TOPO_STAR, 1U, 1U));
    assert(!nr_generate_topology(&n, (NrTopologyType)99, 8U, 1U));
}
static void test_arq_success_and_windows(void) {
    NrNetwork n; NrArqSimulation s; assert(nr_load_demo(&n));
    NrArqConfig c = arq_config(NR_ARQ_STOP_WAIT, 5U, 8U);
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); assert(s.config.window_size == 1U);
    run_arq(&s); assert(s.state == NR_ARQ_COMPLETE && s.stats.generated == 5U && s.stats.delivered == 5U);
    assert(s.stats.retransmissions == 0U && s.stats.acknowledgements == 5U && nr_arq_delivery_ratio(&s) == 1.0);
    c = arq_config(NR_ARQ_GO_BACK_N, 20U, 4U); c.packets_per_second = 10000.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); nr_arq_step(&s, 0.01);
    assert(s.next_sequence == 4U && s.sender_base == 0U);
    c = arq_config(NR_ARQ_SELECTIVE_REPEAT, 6U, 3U);
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_COMPLETE && s.stats.delivered == 6U && nr_arq_efficiency(&s) == 1.0);
    assert(s.history_count > 1U && s.history[s.history_count - 1U].delivery_ratio == 1.0);
}
static void test_arq_errors_and_controls(void) {
    NrNetwork n; NrArqSimulation s; assert(nr_load_demo(&n));
    NrArqConfig c = arq_config(NR_ARQ_SELECTIVE_REPEAT, 8U, 4U); c.loss_probability = 0.28;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_COMPLETE && s.stats.packet_losses > 0U && s.stats.retransmissions > 0U && s.stats.delivered == 8U);
    c = arq_config(NR_ARQ_GO_BACK_N, 12U, 4U); c.corruption_probability = 0.20;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_COMPLETE && s.stats.corruptions > 0U && s.stats.negative_acks > 0U && s.stats.delivered == 12U);
    c = arq_config(NR_ARQ_GO_BACK_N, 5U, 4U); c.corruption_probability = 1.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_FAILED && s.stats.corruptions > 0U && s.stats.negative_acks > 0U && s.stats.retransmissions > 0U);
    c = arq_config(NR_ARQ_STOP_WAIT, 2U, 1U); c.ack_loss_probability = 1.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_FAILED && s.stats.ack_losses > 0U && s.stats.timeouts > 0U);
    c = arq_config(NR_ARQ_SELECTIVE_REPEAT, 3U, 3U); c.ack_corruption_probability = 1.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_FAILED && s.stats.ack_corruptions > 0U);
    c = arq_config(NR_ARQ_STOP_WAIT, 2U, 1U); c.packets_per_second = 0.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); nr_arq_step(&s, 1.0);
    assert(s.state == NR_ARQ_RUNNING && s.stats.generated == 0U); nr_arq_pause(&s); nr_arq_step(&s, 1.0);
    assert(s.state == NR_ARQ_PAUSED && s.stats.generated == 0U); nr_arq_resume(&s); nr_arq_stop(&s);
    assert(s.state == NR_ARQ_STOPPED);
    assert(!nr_arq_start(&s, &n, &c, 999, n.nodes[5].id));
}
static void test_protocol_comparison_and_arq_boundaries(void) {
    NrNetwork n; NrArqSimulation s; assert(nr_load_demo(&n));
    NrArqConfig c = arq_config(NR_ARQ_SELECTIVE_REPEAT, 12U, 4U);
    NrArqComparisonResult gbn, sr;
    assert(nr_arq_compare(&n, &c, n.nodes[0].id, n.nodes[5].id, &gbn, &sr));
    assert(gbn.completed && sr.completed && gbn.statistics.delivered == 12U && sr.statistics.delivered == 12U);
    assert(gbn.duration_seconds > 0.0 && sr.duration_seconds > 0.0 && gbn.efficiency > 0.0 && sr.efficiency > 0.0);
    NrArqSimulation repeat_a,repeat_b;c=arq_config(NR_ARQ_SELECTIVE_REPEAT,12U,4U);c.loss_probability=.18;c.corruption_probability=.12;c.ack_loss_probability=.08;
    assert(nr_arq_start(&repeat_a,&n,&c,n.nodes[0].id,n.nodes[5].id));run_arq(&repeat_a);
    assert(nr_arq_start(&repeat_b,&n,&c,n.nodes[0].id,n.nodes[5].id));run_arq(&repeat_b);
    assert(repeat_a.stats.packet_losses==repeat_b.stats.packet_losses&&repeat_a.stats.corruptions==repeat_b.stats.corruptions&&repeat_a.stats.ack_losses==repeat_b.stats.ack_losses&&repeat_a.stats.delivered==repeat_b.stats.delivered);
    c = arq_config(NR_ARQ_STOP_WAIT, 1U, 1U); c.loss_probability = 1.0;
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_FAILED && s.stats.packet_losses >= 1U && s.stats.retransmissions >= 1U);
    c = arq_config(NR_ARQ_GO_BACK_N, NR_ARQ_MAX_PACKETS, NR_ARQ_MAX_PACKETS);
    assert(nr_arq_start(&s, &n, &c, n.nodes[0].id, n.nodes[5].id)); run_arq(&s);
    assert(s.state == NR_ARQ_COMPLETE && s.stats.delivered == NR_ARQ_MAX_PACKETS);
}
static void test_arq_dynamic_reroute(void) {
    NrNetwork n; NrArqSimulation s; assert(nr_load_demo(&n));
    NrArqConfig c=arq_config(NR_ARQ_SELECTIVE_REPEAT,6U,3U);c.packets_per_second=1000.0;
    assert(nr_arq_start(&s,&n,&c,n.nodes[0].id,n.nodes[5].id));nr_arq_step(&s,.01);
    assert(s.event_count>0U&&nr_set_link_active(&n,4,false));nr_arq_step(&s,.01);
    assert(s.forward_route.reachable&&s.forward_route.length>=5U);
    run_arq(&s);assert(s.state==NR_ARQ_COMPLETE&&s.stats.delivered==6U);
    bool saw_reroute=false;for(size_t i=0;i<s.log_count;i++){size_t ix=(s.log_cursor+128U-s.log_count+i)%128U;if(strstr(s.log[ix],"rerouted on the active graph")!=NULL)saw_reroute=true;}
    assert(saw_reroute);
}
static void test_congestion_control_and_queue_recovery(void) {
    NrCongestionController controller;nr_congestion_init(&controller,64.0);
    assert(nr_congestion_observe(&controller,.01,1.0,true)==32.0&&controller.decreases==1U);
    (void)nr_congestion_observe(&controller,10.0,0.0,false);
    assert(fabs(controller.rate-64.0)<1e-9&&controller.increases>=5U);
    NrNetwork n;NrArqSimulation s;assert(nr_load_demo(&n));
    for(size_t i=0;i<n.node_count;i++)if(n.nodes[i].type==NR_ROUTER)n.nodes[i].queue_capacity=1U;
    NrArqConfig c=arq_config(NR_ARQ_GO_BACK_N,5U,4U);c.packets_per_second=1000.0;c.congestion_control=true;
    assert(nr_arq_start(&s,&n,&c,n.nodes[0].id,n.nodes[5].id));nr_arq_step(&s,.01);
    assert(s.stats.queue_drops>0U&&s.congestion.decreases>0U&&s.queue_utilization==1.0);
    run_arq(&s);assert(s.state==NR_ARQ_COMPLETE&&s.stats.delivered==5U);
    assert(s.history_count>1U&&s.history[s.history_count-1U].queue_utilization>=0.0);
}
int main(void) {
    test_graph_and_routing(); test_packets_and_recovery(); test_cidr(); test_persistence();
    test_empty_and_directional_graphs(); test_loss_ttl_and_queue_boundaries();
    test_node_capacity_and_persistence_errors(); test_cidr_prefix_boundaries(); test_nan_inputs_are_ignored();
    test_topology_templates(); test_arq_success_and_windows(); test_arq_errors_and_controls(); test_protocol_comparison_and_arq_boundaries(); test_arq_dynamic_reroute(); test_congestion_control_and_queue_recovery();
    puts("All NetRescue core tests passed."); return 0;
}
