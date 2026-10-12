# Viva questions and answers

## Architecture

**Q: Which parts are written in C?**
Everything that simulates the network: topology generation, Dijkstra routing, the discrete-event packet
engine with queues and links, the three ARQ protocols, checksums, fault injection, congestion control,
metrics, the experiment runner with statistics, the presets, save/load, the CSV export and the JSON API.
The website (React/TypeScript) only renders engine output and sends settings.

**Q: How does a browser run C code?**
Emscripten compiles the C sources to WebAssembly. The website loads the `.wasm` module in a Web Worker and
calls one exported function, `nr_api_call`, with a JSON string; the C code returns a JSON string.

**Q: How do you know the browser isn't computing results itself?**
The UI has no packet or metric logic; every number is a field of an engine response. The same requests
sent to the native build (`netrescue_api.exe`) return byte-identical results, which is checked by an automated test.

**Q: Why a discrete-event simulation rather than fixed time steps?**
Events are processed exactly at their timestamps in a deterministic order (time, then insertion order).
Fixed steps would merge events inside a step and could reorder frames. The live view still advances by
real time × speed, but inside each step every event fires at its exact time.

## Networking model

**Q: How is delay modelled?**
Per hop: queueing at the router's output port + serialisation (frame bits / bandwidth) + propagation
(link delay) + optional uniform jitter. Data frames are payload + 20 bytes; ACK/NAK frames are 20 bytes.

**Q: How does routing work?**
Dijkstra on link cost (or delay, or a composite). Each node forwards to the next hop of its shortest path to
the destination. When a link fails mid-transfer, frames on it are lost and later frames take the new path.

**Q: What is the no-load RTT?**
Data one way plus ACK back with empty queues, e.g. 4 hops × (5 ms + 0.816 ms) + 4 × (5 ms + 0.016 ms)
≈ 43.3 ms in the ring preset. The engine computes it and uses it in the findings.

## Protocols

**Q: Difference between Go-Back-N and Selective Repeat?**
Go-Back-N: cumulative ACKs, one timer, the receiver accepts only the next expected frame, so after a loss the
sender resends the lost frame and all later outstanding frames. Selective Repeat: individual ACKs, a timer
per frame, the receiver buffers out-of-order frames inside its window, and only the missing frame is resent.
Experiment 5 shows GBN resending 4–11 and SR resending only 4.

**Q: Is Selective Repeat always better?**
No. With ACK loss, Go-Back-N's next cumulative ACK covers a lost ACK, so in experiment 4 it needed almost no
retransmissions while SR and Stop-and-Wait timed out. SR also buffers and delays in-order delivery. The
findings rank protocols by measured values only.

**Q: Why does Stop-and-Wait have low latency but poor goodput?**
Only one frame is ever in flight, so there is no queueing behind other frames (low latency), but the link
idles for a whole RTT per packet: goodput ≤ payload / RTT. The engine reproduces this bound exactly on an idle path.

**Q: How are duplicates prevented from being delivered twice?**
The receiver tracks the next expected sequence number (and a buffer bitmap for SR). A frame below the base, or
already buffered, is counted as a duplicate, re-acknowledged and discarded. Tests assert each sequence number
is delivered at most once and that delivered ≤ requested.

**Q: What happens when the retry limit is reached?**
The transfer is aborted and reported as incomplete (`stop_reason = retry_limit`), with the delivery ratio so far.

**Q: Sequence-number space?**
Sequence numbers are not wrapped modulo 2^k in this simulator (a stated limitation). With k-bit numbers,
Go-Back-N needs W ≤ 2^k − 1 and Selective Repeat W ≤ 2^(k−1), so old and new frames cannot be confused.

## Fault injection

**Q: How do you introduce errors?**
Four probabilities (data loss, data corruption, ACK loss, ACK corruption), per-link loss, queue overflow,
link failures/outages, and deterministic rules such as “drop DATA seq 3 on its first transmission”.

**Q: How is corruption detected?**
The sender computes a 32-bit FNV-1a checksum over the frame bytes. Corruption flips real bits; the
receiver recomputes the checksum over what it received and rejects on mismatch, then sends a NAK (or waits
for the timeout). FNV-1a catches every single-byte error; a multi-byte error escapes with probability ≈ 2⁻³².

**Q: How can a random experiment be reproducible and fair?**
Every random decision is a hash of (seed, sequence, attempt, purpose), not a shared random stream. The same
seed gives the same run, and the k-th copy of packet n has the same fate in every protocol. Trial i uses
seed = base + i × stride for all protocols.

**Q: Configured vs observed faults?**
The configured probability is per transmission; the engine records each actual event in a fault ledger
(time, frame, node/link, type, detected, response, recovered). The UI shows both.

## Metrics

**Q: Goodput vs throughput vs efficiency vs delivery ratio?**
Goodput = unique payload bits delivered / time. Throughput = all bits put on the wire (data + ACKs,
including retransmissions and headers) / time. Efficiency = useful payload bits / all transmitted bits.
Delivery ratio = unique packets delivered / packets requested. A run can have 100 % delivery and low efficiency.

**Q: What does the latency include?**
From the packet's first transmission to its in-order delivery: queueing, serialisation, propagation, every
retransmission round, and for SR waiting in the receive buffer.

**Q: What is an unnecessary retransmission?**
A copy sent after the copy that the receiver eventually accepted, caused by a premature timeout, a lost ACK,
or a go-back resend of a good frame. Experiment 7: below the 163 ms RTT every retransmission is unnecessary.

**Q: How are repeated trials summarised?**
Mean, sample standard deviation and a Student-t 95 % confidence interval per metric; incomplete runs are counted and included.

## Congestion

**Q: How do you create congestion?**
A 1 Mbit/s bottleneck behind 100 Mbit/s links, 5-frame router queues, window 16, 1000 packets/s offered. The
router queue overflows; drops are recovered by the ARQ protocol.

**Q: What does congestion control do here?**
Rate AIMD halves the sending rate on a queue-overflow signal and increases it slowly when the queue is
nearly empty. Window AIMD halves a congestion window on timeouts and grows it per ACK. In experiment 6 both
reduce queue drops compared with no control. It is an educational model, not TCP.

## Testing

**Q: How did you test it?**
Three C suites (the original regression tests, an engine suite with about 30 000 checks including metric
formulas recomputed from the event trace, and an API suite), web tests that run the real WebAssembly engine
and compare it with the native build, UI tests with the real engine, and a headless-browser end-to-end run.
