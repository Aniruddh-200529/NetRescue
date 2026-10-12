# Changes and architecture decisions (version 2.0)

## Decisions

1. **WebAssembly, not a server.** The C engine is compiled with Emscripten and runs in the browser in a Web
   Worker. No backend is needed to use the site; the compiled `.wasm` is committed so only Node.js is required.
   A native host of the same API (`netrescue_api.exe`) exists as the fallback and as a verification tool.
2. **One narrow, versioned JSON entry point** (`nr_api_call`) instead of exporting many C functions. JSON
   is easy to inspect, log, test from C and TypeScript alike, and keeps C structs and pointers private.
3. **The ARQ engine became a true discrete-event simulator.** The original `arq.c` stepped time in fixed ticks,
   modelled a whole route as one delay, treated "events in flight" as queue occupancy, and processed same-tick
   events in an order that could reorder frames. The new engine simulates each hop (queue → serialisation →
   propagation), uses an event heap with deterministic tie-breaking, and keeps every public `nr_arq_*`
   function and statistic of the old API so existing code and tests still work.
4. **Deterministic, keyed randomness** (kept from the original design and extended): fates are hashed from
   (seed, sequence, attempt, purpose), which makes multi-protocol comparisons fair and runs reproducible.
5. **Real checksums.** Corruption flips bits of a reconstructed frame, and detection is the receiver's own FNV-1a comparison.
6. **Findings are generated in C** from the measured aggregates, so the explanation never assumes a winner.
7. **Experiments are self-contained.** A comparison never inherits settings from the live lab; unspecified
   values come from engine defaults.

## Behavioural changes to existing C code

- `max_retries` and `use_nak` are explicit configuration fields (previously a fixed 16 attempts and always-on NAK).
- Queue capacity applies to real per-port FIFO queues on any forwarding node, not to frames in flight.
- A missing route no longer aborts a transfer immediately; frames are dropped (`no_route`) and the retry limit decides.
- Go-Back-N NAKs only the expected frame, once (textbook behaviour), and on a NAK resends from that frame.
- Selective Repeat latency is measured to in-order delivery to the application (was: first reception).
- Sequence numbers in logs are 0-based (the original printed seq + 1).
- Topology generators assign roles (star centre = switch, tree leaves = hosts, ring hosts opposite each other) and use a hierarchical tree layout.

## Files

| File | Status | Summary |
|---|---|---|
| `include/arq.h`, `src/arq.c` | rewritten | Discrete-event engine, ports/queues, checksums, fault rules + ledger, trace, metrics, window/rate AIMD, step-event, run-to-completion |
| `include/netrescue.h`, `src/network.c` | extended | Link jitter, masked routing, `nr_find_link_between`, validation/endpoint and in-memory save/load declarations |
| `src/persistence.c` | rewritten | `NETRESCUE 2` format (jitter, queue capacity), v1 compatible, in-memory text API |
| `src/topology.c` | extended | Device roles and layouts |
| `src/validate.c` | new | Network validation, default endpoints |
| `include/json.h`, `src/json.c` | new | JSON reader/writer |
| `include/experiment.h`, `src/experiment.c` | new | Experiment runner, statistics, findings, CSV |
| `src/presets.c` | new | Seven prebuilt experiments |
| `include/api.h`, `src/api.c` | new | Versioned JSON API |
| `src/main_api.c` | new | Native API host (`netrescue_api.exe`) |
| `src/main_cli.c` | extended | `--list-presets`, `--preset <id>`; default behaviour unchanged |
| `src/main_win32.c` | ported | Draws in-flight frames from the new engine; frees engine buffers |
| `src/congestion.c`, `src/cidr.c` | unchanged | |
| `tests/test_core.c` | minimally adjusted | See testing.md |
| `tests/test_engine.c`, `tests/test_api.c` | new | |
| `CMakeLists.txt` | rewritten | Core library, CLI/API/GUI targets, three tests, Emscripten target |
| `build.bat`, `build.sh`, `start-website.bat`, `scripts/build-wasm.ps1` | new/updated | Windows build, launch and WebAssembly scripts |
| `web/` | new | React + TypeScript website, Web Worker bridge, tests, e2e script |
| `README.md`, `docs/*` | rewritten/new | Documentation, demo guide, viva guide |
