# Testing and verification

## How to run

```bat
build.bat                      :: builds and runs the three C suites through CTest
cd web
npm test                       :: Vitest: WebAssembly integration + UI tests
npm run build && npm run e2e   :: headless Edge/Chrome end-to-end checks of the built site
```

## C suites (CTest)

| Test | File | Content |
|---|---|---|
| `core` | `tests/test_core.c` | Original regression tests: node/link CRUD, duplicate/self links, directed graphs, Dijkstra, inactive elements, packet rerouting, loss/TTL/queue limits, checksum, CIDR /0–/32, persistence success/failure, all topology templates, ARQ success, windows, loss, corruption + NAK, ACK loss/corruption, zero-rate pause/stop, comparison API, link failure rerouting during a transfer, congestion control. Two adjustments for the new engine: the config helper sets `max_retries` and `use_nak` explicitly, and the congestion test adds a 0.1 Mbit/s bottleneck because queues are now real FIFO ports serialised at link bandwidth. |
| `engine` | `tests/test_engine.c` | Clean transfers for all protocols (exact latency and the Stop-and-Wait RTT bound); window never exceeded and SR receive window respected; **GBN vs SR middle-packet loss** (exact retransmitted sets 4..11 vs 4, go-back counts, out-of-order discards vs buffering, in-order delivery); corruption detected by checksum with NAK and with timeout recovery, multi-bit bursts; ACK loss (duplicate suppression, unnecessary retransmission, GBN cumulative cover), ACK corruption; retry limit, zero retries, time limit, too-short timeout; burst loss; link outage with rerouting; queue overflow via queue rule and congestion preset (AIMD reduces drops); random faults on jittered links; **every metric recomputed from the event trace** (counts, bits, goodput, throughput, efficiency, retransmission rate, latency, fault totals, delivered ≤ requested, each sequence delivered at most once); reproducibility and run isolation (bit-identical metrics, protocol subset independence, seed schedule); statistics (mean, sample std. dev., t-based CI); all seven presets and CSV row counts; step-event and controls; invalid configs; topology roles, layouts and validation for every generator and size; disconnected networks; masked routing; save/load v2 and v1. |
| `api` | `tests/test_api.c` | JSON parser/writer edge cases; API errors; every topology command; live simulation through JSON (locking, pause, step, link edits while running, trace-vs-metrics agreement, version-based paging); experiments, presets, CSV, reproducibility, metric definitions. |

## Web suites

| Test | Content |
|---|---|
| `web/tests/engine.integration.test.ts` | Loads the real WebAssembly engine in Node: version, input validation, all generators, disconnected networks, a live transfer whose trace matches its metrics and fault ledger, GBN-vs-SR trace evidence, ACK loss after acceptance, reproducibility, CSV values; **WebAssembly vs native**: three experiments run through both `netrescue_api.exe` and WebAssembly must give byte-identical runs, aggregates and findings. |
| `web/tests/ui.test.tsx` | App shell loads the engine and shows the four modes; protocol controls write the engine configuration; Start/Pause/Step/Stop drive the engine and the UI state equals the engine's state; engine validation errors reach the user; fault-rule editor; form → request mapping; results view renders the engine's findings, evidence, tables and traces; mean ± std. dev. display. |
| `web/scripts/e2e.mjs` | Headless Microsoft Edge on the production build: engine loads, frames are drawn while running, a live transfer completes, a deterministic loss appears in the ledger and log, presets 1/2/4/5 run with the expected evidence, CSV/JSON/report exports are verified against each other, history is saved, no console errors. |

## Results of the final verification run (this build, Windows 11, gcc 16.2, Emscripten 6.0.12, Node 24)

- CTest: `core`, `engine` (30 549 checks), `api` (153 checks): **3/3 passed**, built with `-Wall -Wextra -Wpedantic` and no warnings.
- Vitest: **16/16 passed** (9 integration, 7 UI).
- End-to-end in headless Edge: **13/13 checks passed**.
- A deliberate mutation of one engine-test expectation was confirmed to make the suite fail.

## Not covered

- The Win32 dashboard (`NetRescue.exe`) compiles against the new engine, but it was not exercised interactively.
- The MSVC build path was not executed on this machine; only MinGW gcc was available. The code avoids GCC-only extensions.
- The UI was checked in Chromium-based Edge. Firefox and Safari were not tested.
