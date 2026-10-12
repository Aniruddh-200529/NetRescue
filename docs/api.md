# Engine API (version 1)

One C function, `const char *nr_api_call(const char *request_json)` (`include/api.h`), serves the website
(WebAssembly) and `netrescue_api.exe` (native). Requests are JSON objects with a `cmd` field. Responses:

```json
{"ok": true,  "api_version": 1, "result": { ... }}
{"ok": false, "api_version": 1, "error": "message"}
```

The returned string stays valid until the next call. Numbers that are undefined (zero denominator) are `null`.
Node and link ids are stable integers; no internal pointers cross the interface.

## Session and network

| cmd | Parameters | Result |
|---|---|---|
| `version` | – | engine name, target (`webassembly`/`native`), build date, limits |
| `metric_definitions` | – | `[{key, name, unit, formula, description}]` |
| `get_network` | – | network (below) |
| `load_demo` | – | network |
| `generate_topology` | `type` (`line`,`bus`,`star`,`ring`,`tree`,`mesh`,`hybrid`,`random`), `nodes` 2–64, `seed` | network |
| `add_node` | `type` (`host`,`router`,`switch`,`server`,`access_point`), `name?`, `x`,`y` ∈ [0,1], `queue_capacity?` | network |
| `update_node` | `id`, any of `name`, `type`, `x`, `y`, `queue_capacity`, `active` | network |
| `remove_node` | `id` | network |
| `add_link` | `from`, `to`, `bidirectional?`, `cost?`, `delay_ms?`, `bandwidth_mbps?`, `loss_probability?`, `jitter_ms?` | network |
| `update_link` | `id`, any of the link fields, `active` | network |
| `remove_link` | `id` | network |
| `set_all_links` | any of `delay_ms`, `bandwidth_mbps`, `jitter_ms`, `loss_probability`, `cost` | network |
| `set_all_queues` | `capacity` | network |
| `set_endpoints` | `source`, `destination` | network |
| `set_cost_mode` | `mode`: `link`, `delay`, `composite` | network |
| `export_topology` | – | `{format: "NETRESCUE 2", text}` |
| `import_topology` | `text` | network |

Network: `{nodes, links, source, destination, route, reverse_route, route_cost, cost_mode, locked, base_rtt_ms, issues:[{severity, message, node_id, link_id}]}`.
While a simulation is running or paused (`locked`), structural edits are refused; link/node active flags and
link properties may still change and take effect immediately.

## Live simulation

| cmd | Parameters | Result |
|---|---|---|
| `get_config` / `configure` | `config?` (partial), `scenario?` | `{config, scenario}` |
| `sim_start` | `config?`, `scenario?` + cursors | state |
| `sim_step` | `dt` (simulated seconds) + cursors | state |
| `sim_step_event` | cursors | state (one visible protocol event; works while paused) |
| `sim_pause`, `sim_resume`, `sim_stop`, `sim_reset`, `sim_state` | cursors | state |
| `sim_export` | – | `{trace_csv, faults, metrics, config, scenario}` |

Config keys: `protocol` (`stop_and_wait`,`go_back_n`,`selective_repeat`), `packet_count`, `window_size`,
`payload_bytes`, `packets_per_second`, `timeout_s`, `loss_probability`, `corruption_probability`,
`ack_loss_probability`, `ack_corruption_probability`, `seed`, `max_retries`, `max_sim_time`,
`congestion_mode` (`none`,`rate_aimd`,`window_aimd`), `use_nak`, `corruption_bits`, `cross_traffic_pps`,
`cross_traffic_bytes`. Out-of-range values are rejected with an error naming the key.

Scenario rules: see [fault-injection.md](fault-injection.md); e.g. `{"type":"drop_data","sequence":3,"attempt":1}`,
`{"type":"link_outage","link_id":2,"start":0.1,"end":0.5}`, `{"type":"queue_limit","node_id":3,"capacity":1}`.

Cursors: `trace_from` (first trace index wanted), `trace_limit` (≤ 5000), `fault_version`, `history_version`.
State: `state`, `stop_reason`, `time`, `protocol`, `config`, `sender {base, next, window, effective_window, cwnd, rate}`,
`receiver {base}`, `sender_map` (`n` not sent, `o` outstanding, `a` acked, `f` failed), `receiver_map` (`-`, `b` buffered, `d` delivered),
`attempts_map`, `route`, `reverse_route`, `blocked_links`, `in_flight [{id, kind, seq, ack, attempt, phase, from, to, link, progress, corrupted}]`,
`ports [{link, node, toward, queued, capacity, transmitting, max, drops, forwarded}]`, `metrics`,
`trace [{i, t, type, seq, ack, attempt, frame, node, link, detail, text, cause?, fault_type?, fault_source?}]`, `trace_total`,
`faults` and `history` (only when their version changed).

## Experiments

| cmd | Parameters | Result |
|---|---|---|
| `list_presets` | – | `[{id, title, purpose, settings, what_to_look_for}]` |
| `preset_spec` | `id` | `{id, spec, topology}` |
| `run_preset` | `id` | experiment result |
| `run_experiment` | spec (below) | experiment result |
| `get_experiment` | – | last result |
| `export_experiment_csv` | – | `{csv}` (one row per run) |

Spec: `topology` (`{"kind":"current"}`, `{"kind":"generate","type","nodes","seed"}` or `{"kind":"preset","id"}`),
`source?`, `destination?`, `config` (unspecified keys use engine defaults, not the lab session), `scenario`,
`protocols` (array), `trials` 1–200, `seed_base`, `seed_stride`, `sweep {parameter, values[≤12]}`,
`overrides {link_delay_ms, link_bandwidth_mbps, link_jitter_ms, queue_capacity}`, `capture_trace`.
Sweep parameters: `loss_probability`, `corruption_probability`, `ack_loss_probability`, `ack_corruption_probability`,
`timeout_s`, `window_size`, `packets_per_second`, `queue_capacity`, `link_delay_ms`, `link_bandwidth_mbps`,
`link_jitter_ms`, `congestion_mode` (0/1/2), `payload_bytes`. At most 4000 runs.

Result: `{spec, topology {description, nodes, links, route, route_names, base_rtt_ms}, metric_keys,
runs [{index, sweep_index, sweep_value, protocol, trial, seed, metrics, retransmitted [{seq, count}], captured}],
aggregates [{sweep_index, sweep_value, protocol, runs, completed, stats {metric: {mean, stddev, ci95, min, max, n}}}],
findings [string], captures [{run_index, protocol, sweep_index, trace, faults}]}`.

## Example

```text
> echo {"cmd":"run_preset","id":"gbn_vs_sr"} | build\netrescue_api.exe
{"ok":true,"api_version":1,"result":{"api_version":1,"spec":{...},"runs":[{"protocol":"go_back_n",...,
 "retransmitted":[{"seq":4,"count":1},{"seq":5,"count":1},...,{"seq":11,"count":1}]...
```
