# Metrics

Every metric is computed in C by `nr_arq_compute_metrics()` (`src/arq.c`) from the simulation's own
counters and per-packet state. Aggregates over trials are computed by `nr_experiment_run()`
(`src/experiment.c`). The same definitions are served to the website by the `metric_definitions` API
command and shown in the UI.

Notation: *N* = packets requested, *D* = unique packets delivered in order to the receiving application,
*P* = payload bytes, *T* = elapsed simulation time (completion time, or the time at which the run stopped),
*H* = 20-byte modelled data header, *A* = 20-byte modelled ACK/NAK frame.

| Metric | Formula | Notes |
|---|---|---|
| Packet delivery ratio (PDR) | D / N | Unique packets delivered at least once. Retransmissions and duplicates never count twice. |
| Goodput | D · P · 8 / T | Useful payload only. |
| Throughput (total) | (data-frame bits + ACK/NAK bits) / T | Bits put on the wire by both endpoints, including headers and retransmissions; each frame counted once at its origin, not per hop. |
| Data throughput | data-frame bits / T, data-frame bits = transmissions · (P + H) · 8 | Sender side, includes retransmissions. |
| Average end-to-end latency | mean over delivered packets of (in-order delivery time − first transmission time) | Includes serialisation, propagation, jitter, queueing, every timeout/retransmission round and, for Selective Repeat, waiting in the receive buffer for earlier gaps. Also min, max and sample std. dev. |
| Completion time | time of the final ACK | Incomplete runs report the stop time and `completed = false` with `stop_reason` (`retry_limit`, `time_limit`, `capacity`, `user`). |
| Retransmission count | data transmissions − first transmissions | Split into timeout-, NAK- and go-back-caused. The receiver never retransmits ACKs on its own; ACKs sent in response to duplicate/out-of-order data are reported as `dup_acks_sent`. |
| Retransmission rate | retransmissions / first transmissions | |
| Unnecessary retransmissions | Σ over received packets of (attempts − attempt number of the copy the receiver accepted) | Copies sent after the copy that eventually got through, e.g. premature timeouts, lost ACKs, go-back resends of already-good frames. |
| Protocol efficiency | D · P · 8 / (data-frame bits + ACK/NAK bits) | Fraction of transmitted bits that were useful payload. Not the same as PDR. |
| Frame efficiency | D / data transmissions | |
| Fault statistics | per fault type: injected, detected, recovered, unrecovered, no-impact | See [fault-injection.md](fault-injection.md). |
| Queue occupancy | time-average of waiting frames per forwarding output port | Mean over used router/switch ports and the worst port; peak occupancy; endpoints' own transmit buffers excluded. |
| Queue drops | frames arriving at a full output queue | Data and ACK drops reported separately. |
| Congestion events | multiplicative decreases / additive increases | Plus minimum rate and minimum window reached. |
| Average RTT | mean of (ACK time − send time) for packets sent exactly once | Karn's rule: retransmitted packets give no RTT sample. |

**Zero denominators:** when a denominator is zero the engine sets a `has_…` flag to false and the JSON value is
`null` (e.g. latency when nothing was delivered, goodput when no time elapsed). The UI shows `n/a`.

**Aggregates:** for each (sweep value, protocol) the runner reports `mean`, sample standard deviation
(n − 1), 95 % confidence half-width `t(0.975, n−1) · s / √n` (Student t table; 1.96 beyond 120 degrees of
freedom), `min`, `max` and `n` (the number of runs where the metric is defined). Incomplete runs are
included in the averages and counted in `completed / runs`.

**Analytic check used in tests:** on an idle path, Stop-and-Wait completion time equals N × no-load RTT, and its
goodput equals P · 8 / RTT. The `engine` test suite asserts this exactly.
