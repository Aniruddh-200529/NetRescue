# Fault injection

All fault injection is implemented in C in `src/arq.c`. It is configured from both website modes, the
JSON API, and C (`NrArqConfig`, `NrFaultScenario`).

## Where faults happen in a frame's lifecycle

A frame is created by the sender (DATA) or receiver (ACK/NAK), queued at its origin's output port,
serialised at the link bandwidth, propagates for the link delay (+ jitter), and is then either delivered
(if this node is its destination) or queued at the next router's port toward the destination.

| Fault | Stage | What the engine does |
|---|---|---|
| Random data/ACK loss | on a chosen hop of the route | At creation the frame's fate is drawn; the frame then vanishes when it reaches the end of that hop (trace `DATA_LOST`/`ACK_LOST` with node and link). |
| Random data/ACK corruption | on a chosen hop | The frame's bits are flipped when it crosses that hop (`CORRUPTION_INJECTED`); it continues travelling and is checked at its destination. |
| Per-link loss | each traversal of that link | Independent draw per link, per frame copy. |
| Queue overflow | arrival at a router | If the output queue already holds `queue_capacity` waiting frames, the frame is dropped (`queue_overflow`). |
| Link outage / failed link or node | while on or entering the link | Frames propagating on a link that goes down are lost (`link_outage`); routing recalculates; frames with no route are dropped (`no_route`). |
| Deterministic rules | middle hop of the route | Exact frames are dropped or corrupted (see below). |

## Random model and reproducibility

Every random decision is a stateless hash of `(seed, sequence number, attempt number, purpose)`.
There is no shared random stream, so:
- the same seed and configuration always produce the same run (tests verify bit-identical metrics);
- the k-th transmission of packet n meets the same fate in every protocol, which makes comparisons fair even
  though protocols send different numbers of frames. ACK fates are keyed by `(seed, sequence, n-th ACK sent for it)`.

Experiment trial i uses seed `seed_base + i × seed_stride` for every protocol and sweep value.

## Deterministic scenarios

| Rule | Parameters | Example |
|---|---|---|
| `drop_data` | sequence, attempt (1 = first send, 0 = every send) | drop seq 3 on its first transmission |
| `corrupt_data` | sequence, attempt | corrupt seq 5 on its first transmission |
| `drop_ack` | sequence, attempt = which ACK/NAK sent for that sequence (1 = first) | drop the first ACK for seq 3 |
| `corrupt_ack` | sequence, attempt | |
| `burst_loss` | sequence, count, attempt | drop seq 10..14 on first transmission |
| `link_outage` | link_id, start, end (simulated seconds) | link 2 down from 0.1 s to 0.5 s |
| `queue_limit` | node_id, capacity | force R2's queue to 1 frame |

Rules and random probabilities can be combined.

## Checksum model

Each frame carries a 32-bit FNV-1a checksum computed by the sender over the frame's bytes: kind,
sequence, ack and length fields plus the payload (payload bytes are generated deterministically from the
seed and sequence number). Corruption flips a burst of `corruption_bits` consecutive bits (default 1, up to
64) at a pseudo-random position: in the payload for data frames, in the sequence/ack fields for ACK/NAK
frames. The receiver (or sender, for ACKs) recomputes the checksum over the bytes it actually received and
compares. Detection is therefore a real computation, not a flag.

- FNV-1a detects every change confined to one byte (each step is a bijection of the 32-bit state).
- For larger bursts an undetected error is possible with probability about 2⁻³². The engine counts
  `undetected_corruptions` if one ever passes. None occurred in the test suites.
- The data header is assumed protected by the link layer, so on a checksum failure the receiver can NAK the
  correct sequence number. With NAK disabled, corrupted frames are silently discarded and recovered by timeout.

## Recorded for every fault

Time, frame kind and id, sequence, attempt, node, link, fault type, source (`random`, `link_probability`,
`scenario`, `congestion`, `topology`), detected (and when), protocol response, recovered (and when).

- **Detected:** a corruption caught by the checksum, or a loss the sender reacted to (timeout, NAK, or go-back retransmission).
- **Response:** `nak_sent`, `discarded_await_timeout`, `retransmitted_after_timeout`, `retransmitted_after_nak`,
  `retransmitted_go_back`, `covered_by_later_ack` (Go-Back-N cumulative ACK), `no_impact`, `undetected`.
- **Recovered:** a data fault when the packet is later received correctly; an ACK fault when the packet is later acknowledged.
- **No impact:** a lost copy of data the receiver already had, or of an ACK for data already acknowledged.

The UI's fault summary shows configured probabilities next to the observed counts.
