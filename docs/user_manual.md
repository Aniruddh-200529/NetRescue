# User manual

## Launch and first transfer

Build the Windows dashboard using the README commands and launch `NetRescue.exe`. The initial six-node graph has redundant paths and uses seed `424242`. The default transfer uses Go-Back-N, 20 packets, a four-packet window, and 20 packets/second. Choose a protocol in the right inspector and use **Start** or **Run Traffic** to run the batch.

## Topologies and editing

Click a left-sidebar topology to replace the graph with a generated connected topology. The templates are Line, Bus, Star, Ring, Tree, Mesh, Hybrid, and Random. Click a node to select it; drag it on the canvas to reposition it. Add node controls append a selected node type and connect it to the selected node. To add a custom link, select a node, press `L`, and select its peer. `Delete` removes the selected node or link. Editing and topology replacement are disabled during active or paused transfers to keep simulator references valid.

Click a link to select it. **Fail Link** / `F` toggles its active state; a failed edge is rendered red and routing recalculates from the graph. Select a router or switch and press `N` to toggle its active state. `F2` renames a selected node; `T` cycles its device type. `R` resets the transfer and restores all failed nodes/links. `F5` restores the original demo topology.

## Transfer configuration

Select a node and press `1` to choose the source or `2` to choose the destination. The inspector shows the endpoints. Click the protocol row to cycle Stop-and-Wait, Go-Back-N, and Selective Repeat. Click the left/right side of inspector rows to decrement/increment window size, data loss, ACK loss, packet count, data corruption, ACK corruption, timeout, and payload bytes. The window is bounded to 1–64 and packet count to 1–512. `[` / `]` also changes payload size; `+` / `-` changes traffic rate; `G` chooses a new seed. A fixed seed is displayed in the inspector and makes a run repeatable.

**Start** begins a batch; it changes to Pause and Resume as appropriate. **Run Traffic** starts the configured batch and changes to Stop Traffic while active. `S` starts a transfer; `Space` pauses/resumes. A zero rate deliberately generates no packets. Retry limits and finite packet/event buffers stop pathological runs safely.

The active route's router/switch queue capacities bound in-flight data. Transmissions that exceed the queue are dropped and recovered through normal ARQ retries. Press `M` to toggle the educational congestion controller: it halves the offered rate on overflow, then additively increases the rate as the queue drains. Queue utilization is available in the inspector and chart.

Press `V` to run Go-Back-N and Selective Repeat comparisons with the current topology, endpoints, packet settings, and seed. Results include packet transmissions, retransmissions, duration, and efficiency; the comparison is also written to the event log. Click the chart to cycle delivery ratio, throughput, average latency, queue utilization, and packet loss rate.

## Save and load

`Ctrl+S` opens a save dialog for the current graph; `Ctrl+O` loads a versioned `.topo` file. Invalid files are rejected without replacing the active graph. Load is disabled during an active or paused transfer.

## CIDR and CLI

The C API `nr_parse_cidr` parses IPv4 prefixes `/0` through `/32`; `nr_format_ipv4` formats addresses. The command-line program runs the deterministic demonstration and prints its selected Dijkstra path.
