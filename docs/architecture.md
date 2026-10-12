# Architecture

```
 Browser                                                     C engine (WebAssembly, in a Web Worker)
 ┌──────────────────────────────┐   JSON request string     ┌───────────────────────────────────────────┐
 │ React UI (web/src)           │ ────────────────────────▶ │ api.c      nr_api_call(json) → json        │
 │  - Network Simulation lab    │                           │ json.c     parser / writer                 │
 │  - Protocol Comparison lab   │ ◀──────────────────────── │ experiment.c / presets.c  fair runner      │
 │  - History, Help             │   JSON response string    │ arq.c      discrete-event ARQ + faults     │
 │ EngineClient → engine.worker │                           │ network.c  graph + Dijkstra                │
 └──────────────────────────────┘                           │ topology.c validate.c persistence.c ...    │
                                                            └───────────────────────────────────────────┘
```

**Rule:** packet outcomes, routing, retransmissions and metric values are produced only by C. The
TypeScript code formats engine output (units, tables, chart rows) and sends user settings. The engine's
`metric_definitions`, findings and CSV export are also produced in C.

## C modules

| File | Responsibility |
|---|---|
| `network.c` / `netrescue.h` | Bounded graph (64 nodes, 256 links), node/link CRUD, Dijkstra by link cost, delay or composite cost, masked routing (ignores links in an outage), FNV-1a checksum, original hop-by-hop packet simulator (`nr_step`, used by the CLI demo and Win32 dashboard) |
| `topology.c` | Line, bus, star, ring, tree, full-mesh, hybrid and seeded-random generators with device roles and layouts |
| `validate.c` | Validation (unreachable endpoints, reverse path, disconnected components, isolated nodes, zero bandwidth) and default endpoints |
| `persistence.c` | `NETRESCUE 2` text format (v1 still accepted), file and in-memory save/load, atomic replace on success only |
| `arq.c` / `arq.h` | Discrete-event reliable-transfer engine: event heap, frame pool, per-port FIFO queues, serialisation + propagation + jitter, Stop-and-Wait / Go-Back-N / Selective Repeat, timers, NAK, checksums, fault rules and probabilities, fault ledger, event trace, congestion control, history samples, metrics |
| `congestion.c` | AIMD rate controller (unchanged from the original project) |
| `experiment.c` / `experiment.h` | Experiment runner (sweeps × protocols × trials), aggregation (mean, std. dev., 95 % CI), evidence, findings, CSV |
| `presets.c` | The seven documented experiments, each with its own topology |
| `json.c` | Minimal JSON reader/writer |
| `api.c` / `api.h` | Versioned JSON API and the session (network, endpoints, configuration, live simulation, last experiment) |
| `cidr.c` | IPv4 CIDR arithmetic (unchanged) |
| `main_cli.c`, `main_api.c`, `main_win32.c` | CLI demo/preset runner, native API host, original Win32 dashboard |

## Discrete-event core (arq.c)

Events are kept in a binary min-heap ordered by (time, insertion order), so simultaneous events are
processed deterministically in FIFO order. Event kinds: sender wake-up (pacing), transmission done,
arrival at a node, per-packet timeout, Go-Back-N timeout, 10 ms monitor (congestion control and history),
outage start/end, cross-traffic arrival.

`nr_arq_step(dt)` processes every event up to now + dt (used by the live animation); `nr_arq_step_event`
processes the next visible protocol event (used by Step); `nr_arq_run` runs to completion or a stopping
criterion (used by experiments). Timers use tokens, so a restarted or cancelled timer's old event is ignored.

Each directed link end is a **port** with a FIFO queue. The frame at the head is serialised for
`bits / bandwidth` seconds, then propagates for `delay_ms (+ jitter)`. Router/switch ports enforce the
node's `queue_capacity` (waiting frames); the two endpoints' own transmit buffers are bounded at 256 frames.
At every node the next hop is taken from Dijkstra toward the frame's destination (cached; recomputed
when the topology changes), so failures reroute traffic hop by hop.

## Website (web/src)

| Path | Role |
|---|---|
| `engine/generated/` | Emscripten output (`netrescue_engine.mjs` + `.wasm`) and its type declaration |
| `engine/engine.worker.ts` | Loads the engine in a Web Worker and forwards JSON strings |
| `engine/client.ts` | Promise-based, serialised `call(cmd, params)` |
| `engine/types.ts` | TypeScript mirror of the API's JSON |
| `state/labStore.ts` | Zustand store for the lab; accumulates the engine's trace pages |
| `pages/lab/*` | Mode A: canvas (React Flow), topology panel, inspector, controls, live views |
| `pages/compare/*` | Mode B: presets, experiment form, results (Recharts) |
| `lib/report.ts` | Standalone HTML report built from the engine result |
| `lib/history.ts` | IndexedDB experiment history |

The animation loop calls `sim_step` with `dt = real elapsed time × speed` on every animation frame
while running; the response carries the in-flight frames (hop, progress, kind, corrupted), queue
occupancy, sender/receiver maps, metrics and new trace records. Faults and history are only resent when
their version counters change.

## Fallback

`netrescue_api.exe` hosts the identical API natively (JSON lines on stdin/stdout). The web tests check that
the WebAssembly and native builds return byte-identical experiment results.
