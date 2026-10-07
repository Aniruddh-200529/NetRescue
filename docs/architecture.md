# Architecture

`netrescue_core` contains the portable graph and simulation logic. It has no windowing or drawing calls. `NrNetwork` owns bounded arrays for nodes, links, packets, counters, the reproducible random state, a circular event log, and tick-based packet progress. Edges are scanned during Dijkstra relaxations.

`nr_find_route` runs Dijkstra using the configured edge cost. It ignores inactive nodes and links and respects directed edges. `nr_step` advances simulation time and packet progress; a missing next edge triggers a fresh route calculation from the packet's current node. The GUI reads this same state and draws nodes, edges, packets, counters, and events.

`cidr.c` is independent IPv4 address arithmetic. `persistence.c` parses a line-oriented versioned topology into a temporary network and only replaces the caller's state on success. `main_win32.c` is the Windows GDI front end. `main_cli.c` is the portable deterministic demo.
