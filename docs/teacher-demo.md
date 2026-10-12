# Teacher demonstration guide (about 15 minutes)

Before the session: run `start-website.bat` (or `cd web` → `npm run dev`) and open <http://localhost:5173>.
Optionally build the C side (`build.bat`) to show the tests. Numbers quoted below are what this build of
the engine produced during verification; the seeded scenarios are deterministic, so you should see the same values.

## 1. Show that the core is C (1 min)

- Point to the top-right badge: **“C engine · webassembly · API v1”**.
- Open *Help / About → What runs where*. Explain: the C files in `src/` are compiled with Emscripten into
  `netrescue_engine.wasm`, which runs in a Web Worker; React only draws the JSON the engine returns.
- Optional: in a terminal, `build\NetRescueCLI.exe --preset gbn_vs_sr` runs the same experiment natively and
  prints the same evidence. The web test suite checks that native and WebAssembly results are byte-identical.

## 2. Build a network and route (2 min)

1. *Network Simulation* → Template **Ring**, Nodes **8** → *Generate*. Hosts H1 and H5 are the endpoints.
2. Show the **Dijkstra route**, the ACK route and the no-load RTT under *Transfer endpoints*.
3. Click a link on the route → *Fail link*. The route changes immediately (computed by C). *Recover link*.
4. Mention validation: add a *Server* without links and set it as destination: the engine reports
   “Destination is unreachable…” and *Start* is blocked. Set the destination back to H5.

## 3. A clean transfer, slowly (2 min)

1. *Protocol & traffic*: **Go-Back-N**, 12 packets, window 4. Speed **0.05×**. Press **Start**.
2. Blue circles are DATA frames with their sequence number, green tags are ACKs (`A3` = cumulative ACK 3).
3. Open *Sender / receiver windows*: the highlighted cells are the sending window sliding forward.
4. Press **Pause**, then **Step** a few times: each click executes the next engine event and logs it.

## 4. “How do you introduce errors?” — deterministic loss (3 min)

1. *Fault injection* → *Add deterministic fault*: **Drop DATA packet**, sequence **3**, attempt **1** → *Add fault rule*.
2. Protocol **Selective Repeat**, press **Start** at 0.05×.
3. A red ✕ marks where seq 3 vanished (on the middle hop). Seqs 4, 5, … arrive and turn **violet/buffered**
   in the receiver window. After the timeout, seq 3 is retransmitted (violet circle); the buffered packets are then delivered in order.
4. *Fault ledger*: one row with time, DATA seq 3 attempt 1, node/link, **detected yes**, response
   “retransmitted after timeout”, **recovered yes**.
5. *Event log* → filter “Faults & timeouts”: the loss, the timeout and the retransmission.

Repeat with **Go-Back-N**: after the timeout the sender resends 3 **and every later outstanding packet**,
because the Go-Back-N receiver discarded them as out of order.

## 5. Corruption and checksum (1 min)

Remove the rule (✕), add **Corrupt DATA packet**, sequence 5, attempt 1, Start. The frame turns yellow
after bits are flipped; the receiver's FNV-1a checksum fails (“Checksum mismatch … NAK sent”), the NAK
(orange) triggers the retransmission. Untick “Receiver sends NAK” to show recovery by timeout instead.

## 6. ACK loss (1 min)

Rule **Drop ACK**, sequence 3, ACK #1, protocol **Stop-and-Wait**. Point out in the log: “Receiver accepts
DATA seq 3” happens *before* “ACK for seq 3 lost”. The sender times out, resends, and the receiver logs
“Duplicate DATA seq 3 discarded and re-acknowledged (no duplicate delivery)”. Metrics: delivered stays 12/12.

## 7. Protocol Comparison mode (4 min)

Open *Protocol Comparison* (purple, separate page).

1. **1. Ideal network → Run.** All three protocols deliver 100 % with zero retransmissions. Stop-and-Wait
   needs about 4.33 s versus 0.57 s for the windowed protocols; its goodput exactly matches the
   one-packet-per-RTT ceiling shown in the findings.
2. **5. Go-Back-N versus Selective Repeat → Run.** *Retransmission evidence* tab: Go-Back-N retransmitted
   **4, 5, 6, 7, 8, 9, 10, 11**; Selective Repeat retransmitted only **4**. *Captured traces*: filter `LOST`
   and `RETX` to show the actual events.
3. **2. Increasing data loss → Run** (150 runs, 10 seeded trials each). The charts show mean ± 95 % CI.
   At 30 % loss Go-Back-N made about 335 retransmissions per run against about 45 for Selective Repeat.
   Note that Stop-and-Wait has the lowest latency per packet: the findings rank by measured values and do
   not assume a winner.
4. Export: **Export CSV**, **Export JSON**, **Download report**. Open the HTML report.
5. Optionally **6. Congestion** (queue drops fall with AIMD) and **7. Timeout sensitivity** (timeouts below
   the 163 ms RTT make every retransmission unnecessary).

## 8. Tests (1 min, optional)

`build.bat` runs the C suites (`core`, `engine` with ~30 000 checks, `api`). `cd web` → `npm test` runs the
WebAssembly integration and UI tests. `npm run e2e` drives the built site in a headless browser.
