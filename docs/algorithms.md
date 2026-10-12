# Algorithms

## Routing

Dijkstra maintains tentative distances and predecessors, repeatedly selects the unvisited minimum and relaxes
each traversable edge. Edge weight is the link cost, the delay, or `cost + delay·0.1 + utilisation·2`
(composite). Inactive nodes and links, and links blocked by a scheduled outage (`nr_find_route_masked`),
are excluded; directed links are respected. In the ARQ engine every node forwards a frame to the next hop
of the shortest path toward the frame's destination, so a failure mid-transfer reroutes traffic at the node
where the frame currently is.

## Stop-and-Wait

Window of one. The sender transmits packet *n*, starts its timer and waits. The receiver accepts only the
next expected packet, delivers it, and returns a cumulative ACK carrying the next expected number. A
duplicate (the ACK was lost or the timer was premature) is discarded and re-acknowledged. On a timeout or a
NAK the same packet is resent.

## Go-Back-N

Sender window *W*, one timer for the oldest unacknowledged packet. The receiver accepts only the next
expected packet; anything later is discarded and answered with a duplicate cumulative ACK. A cumulative ACK
*k* acknowledges everything below *k*; it restarts the timer if packets remain outstanding. **On timeout the
sender resends every outstanding packet from the base** (the first is counted as a timeout retransmission,
the rest as go-back retransmissions). On a NAK for packet *k* it resends from *k*. The receiver sends at most
one NAK per expected packet. A lost ACK is usually harmless because the next cumulative ACK covers it.

## Selective Repeat

Sender window *W* with **one timer per packet**; each ACK acknowledges exactly one packet. The receiver
accepts any correct packet inside `[base, base + W)`, buffers out-of-order packets, re-acknowledges
duplicates, and delivers to the application in order when gaps fill. Only packets whose own timer expired,
or that were NAKed, are resent.

## Timers and limits

The timer starts when a frame is handed to the sender's transmit queue. Each packet may be retransmitted
`max_retries` times; one more need aborts the whole transfer (`retry_limit`). `max_sim_time` (default 600 s)
stops runaway runs (`time_limit`). Both are reported as incomplete runs.

## Pacing, windows and congestion control

New packets are released at most every `1 / packets_per_second` seconds and only while the window has room.
Rate AIMD (the original `congestion.c` controller): on a queue-overflow signal from the path, or route queue
utilisation ≥ 80 %, the offered rate is halved (at most once per timeout interval); while utilisation stays
≤ 25 % it rises by 10 % of the maximum each second. Window AIMD: the congestion window starts at 1, grows by
1 per ACK below `ssthresh` and by 1/cwnd above it, and halves on a timeout (at most once per timeout interval).
The effective window is `min(W, floor(cwnd))`.

## Faults and checksums

See [fault-injection.md](fault-injection.md).

## Experiments

For every sweep value × selected protocol × trial the runner copies the network, applies overrides and the
sweep value, builds a fresh simulation with seed `seed_base + trial × seed_stride` and runs it to the end.
Statistics use the sample standard deviation and Student-t 95 % intervals. The findings are sentences built
from the aggregates: rankings at the first and last sweep points (ties when equal at display precision;
CI overlap noted), Go-Back-N vs Selective Repeat retransmission split, unnecessary retransmissions compared
with the timeout and no-load RTT, the Stop-and-Wait one-packet-per-RTT ceiling, observed faults, sweep
trends, and queue/congestion behaviour. No conclusion is hard-coded.

## Topology templates

Line/bus: chain. Star: centre switch, hosts around it. Ring: cycle with hosts opposite each other. Tree: binary
tree, router root, switches inside, hosts at the leaves. Full mesh: every pair (at most 23 nodes, within the
256-link limit). Hybrid: chain plus chords every second node plus an end-to-end link. Random: spanning chain
plus seeded extra edges (28 %), so it is always connected.

## CIDR

IPv4 values are 32-bit integers; prefix masks are formed without shifting by 32. `/31` exposes both
addresses as point-to-point endpoints; `/32` is one host route.
