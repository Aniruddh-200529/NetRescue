import { useLab } from '../state/labStore';

export function HelpPage() {
  const { definitions, version } = useLab();
  return (
    <div className="help">
      <section>
        <h2>What runs where</h2>
        <p>All networking logic is C. The website loads the NetRescue C engine compiled to WebAssembly (Emscripten) inside a Web Worker and talks to it through one JSON function, <code>nr_api_call</code>. The engine builds topologies, runs Dijkstra routing, simulates every frame hop by hop through bounded queues and links, injects faults, runs the Stop-and-Wait, Go-Back-N and Selective Repeat state machines, computes every metric, runs the comparison experiments and writes the CSV export. The React interface only draws what the engine returns and sends your settings back.</p>
        {version && <p className="small muted">Loaded: {version.engine} · target {version.target} · API v{version.api_version} · built {version.built}</p>}
      </section>
      <section>
        <h2>Network Simulation lab</h2>
        <ol>
          <li>Generate a topology (line, bus, star, ring, tree, full mesh, hybrid, seeded random) or build one: add devices, select a node and use “Connect selected node”, drag nodes to arrange them.</li>
          <li>Pick the source and destination. The Dijkstra route and the no-load round-trip time are computed by the engine.</li>
          <li>Choose the protocol, window, timeout, packet count and offered rate. Add random impairments and/or deterministic fault rules.</li>
          <li>Press Start. Use the speed slider for slow motion, Pause + Step to advance one engine event at a time, and fail links during the transfer to watch rerouting.</li>
          <li>Read the event log, fault ledger (each fault with its time, frame, node/link, detection, protocol response and recovery), sliding-window view, live charts and metrics.</li>
        </ol>
        <p><b>Frame colours:</b> blue = data, violet = retransmitted data, green = ACK, orange = NAK, yellow = corrupted frame (bits already flipped), red ✕ = loss or drop. ACKs travel on the opposite side of the link from data.</p>
      </section>
      <section>
        <h2>How faults are injected</h2>
        <ul>
          <li><b>Random probabilities</b> (data loss, data corruption, ACK loss, ACK corruption) are decided per transmission from a counter-based hash of (seed, sequence number, attempt number). The same transmission meets the same fate in every protocol run with the same seed, which makes comparisons fair and runs repeatable.</li>
          <li><b>Deterministic rules</b> target exact frames: drop or corrupt DATA seq N on attempt A, drop or corrupt the A-th ACK sent for seq N, burst losses, a temporary link outage between two times, or a forced small queue at a router.</li>
          <li><b>Corruption</b> flips real bits of the frame (a burst of 1–64 consecutive bits). The receiver recomputes the FNV-1a 32-bit checksum over header + payload and rejects the frame on mismatch; a corrupted ACK is rejected by the sender the same way. FNV-1a detects every single-byte change; for multi-byte damage the chance of an undetected error is about 2<sup>-32</sup>. The data header is assumed to be protected by the link layer, so the receiver can NAK the right sequence number.</li>
          <li><b>Per-link loss</b>, <b>queue overflow</b> (bounded FIFO at each router output port, serialised at link bandwidth) and <b>link failures</b> are further fault sources.</li>
        </ul>
      </section>
      <section>
        <h2>Metric definitions (from the engine)</h2>
        {definitions.map((d) => (
          <div className="def" key={d.key}>
            <b>{d.name}</b> <span className="muted">({d.unit})</span>
            <div><code>{d.formula}</code></div>
            <div className="small secondary">{d.description}</div>
          </div>
        ))}
      </section>
      <section>
        <h2>Model limits</h2>
        <ul>
          <li>One reliable transfer at a time (plus optional constant-rate cross traffic on the first forwarding hop). Sequence numbers are not wrapped modulo 2<sup>k</sup>.</li>
          <li>The ACK timer starts when a frame is handed to the sender's own transmit queue; the retry limit aborts the whole transfer (reported as incomplete).</li>
          <li>Congestion control is educational AIMD (rate-based on explicit queue-overflow signals, or window-based on timeouts), not a full TCP implementation.</li>
        </ul>
      </section>
    </div>
  );
}
