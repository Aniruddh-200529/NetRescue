# Algorithms

## Routing

Dijkstra maintains tentative distances and predecessors, repeatedly selects the unvisited minimum, then relaxes each traversable graph edge. The selected cost is link cost, delay, or `cost + delay * weight_delay + utilization * weight_congestion`. Inactive endpoints and inactive edges are excluded. The result contains reachability, ordered node IDs, and aggregate cost.

## Packet progression and recovery

Each packet stores a path, current hop, TTL, and fractional hop progress. `nr_step` advances fractional progress according to edge delay and simulation delta time. On an unavailable edge, it computes a new shortest path from the packet's current node. Link loss is drawn from a seeded LCG and compared to the link's configured probability.

## Reliable transfer

`arq.c` owns a timer-driven event simulation independent of the UI. Data and ACK/NACK events have route delay, sequence/ack numbers, attempt count, checksum result, loss/corruption state, and start/due times. Stop-and-Wait limits the sender to one outstanding frame. Go-Back-N uses a cumulative receiver base and retransmits the outstanding window when the base times out or a NACK arrives. Selective Repeat ACKs and buffers in-window out-of-order frames independently and retries only unacknowledged timed-out frames. All protocols bound the retry count and event/frame capacities. Corruption changes checksum input; a mismatch leads to a NACK or discarded ACK, then retransmission/timeout behavior.

Traffic generation advances from simulation time at the configured packets-per-second rate, bounded by the sender window. Error draws are deterministically keyed by seed, sequence, attempt, and error type, so ACK-count differences do not shift later data-frame loss/corruption patterns. `nr_arq_compare` runs GBN and Selective Repeat with the same configuration and seed, returning actual duration, transmissions, delivery, retries, and efficiency. Chart history samples are calculated from delivered packets, payload bytes, elapsed time, and accumulated delivery latency.

## Queueing and congestion control

The active forward route's smallest router/switch queue capacity bounds simultaneous data events. Attempts arriving while it is full are marked dropped and proceed through the selected ARQ protocol's normal timeout/retry path. The optional controller applies multiplicative decrease on overflow or utilization at/above 80%; while utilization stays at/below 25%, it additively increases the configured packet rate each second until it reaches the original rate. The model is educational and does not implement TCP.

## Topology templates

`topology.c` creates graph nodes and links through the same network model used by routing. The templates generate a path-like line/bus graph, star, ring, binary tree, full mesh, hybrid chords, or seeded random redundant edges. The random template first creates a spanning line to keep all generated nodes reachable.

## Checksum and reliability

`nr_checksum` uses 32-bit FNV-1a over the provided byte range. The reliability value is a project-defined score: delivered/generated percentage less 25 points per lost or dropped packet as a fraction of generated traffic, bounded to 0–100.

## CIDR

IPv4 values are represented as 32-bit network-order integers. Prefix masks are formed without shifting by 32. `/31` exposes both addresses as point-to-point endpoints; `/32` represents one host route.
