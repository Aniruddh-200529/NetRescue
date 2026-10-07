# NetRescue

NetRescue is a native C11 network reliability lab with a Windows NOC-style desktop dashboard and portable CLI. A shared graph and simulation engine drives Dijkstra routing, packet progression, topology generation, reliability protocols, error recovery, statistics, and the dashboard visualization. The supplied image was used as visual direction for a dark, high-contrast network operations layout; the interface and topology are original.

## Features

- Bounded graph model with hosts, routers, switches, servers, wireless APs, directed/bidirectional links, queues, link delay/bandwidth/loss, and Dijkstra routing by cost, delay, or weighted composite cost.
- Line, bus, star, ring, tree, full mesh, hybrid, and seeded random topology templates. Drag nodes, add/delete nodes, add/delete links, fail/recover links and nodes, and select transfer endpoints.
- Stop-and-Wait, Go-Back-N, and Selective Repeat ARQ with sliding windows, sequence numbers, checksum corruption detection, ACK/NACK, loss, timeouts, retransmission, receiver buffering, duplicate/out-of-order handling, and bounded retry failure.
- Finite rate traffic batches, pause/resume/stop, configurable packet count/rate/window/timeout/data and ACK loss/corruption, fixed or regenerated seed, and fair-seed GBN/SR comparison.
- Bounded forwarding queues on the selected route. Queue overflow is a real data drop and triggers ARQ recovery. Optional educational congestion control halves the offered rate on congestion and gradually increases it as the queue clears.
- Live topology and packet/ACK animation, event log, delivery/throughput/latency/queue/loss charts, transmission/retry/loss statistics, and protocol efficiency metrics.
- Existing hop-by-hop packet simulator with link/node fault recovery, queue overflow drops, congestion/utilization, checksum utility, project-defined reliability score, CIDR `/0` through `/32`, and versioned topology save/load.
- Native Win32/GDI dashboard in C. No JavaScript, web stack, Python runtime, or external GUI library is used by the application.

The reliability score in the graph simulator is project-defined (delivery ratio with a loss/drop penalty), not an industry-standard metric. The ARQ dashboard's displayed reliability is the measured delivery ratio for the selected transfer.

## Build and run

Requires CMake 3.16+ and a C11 compiler. No external libraries are needed for the simulator; the Windows UI uses system Win32 libraries.

### Windows

With MSVC Build Tools:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\NetRescue.exe
```

This workspace also has a portable local toolchain (CMake 4.4.4, Ninja 1.13.2, Zig 0.16.0):

```powershell
$root = (Get-Location).Path
& .netrescue-tools\cmake\data\bin\cmake.exe -S $root -B "$root\build" -G Ninja `
  -DCMAKE_MAKE_PROGRAM="$root\.netrescue-tools\bin\ninja.exe" `
  -DCMAKE_C_COMPILER="$root\.netrescue-tools\ziglang\zig.exe" `
  -DCMAKE_C_COMPILER_ARG1=cc -DCMAKE_BUILD_TYPE=Release
& .netrescue-tools\cmake\data\bin\cmake.exe --build "$root\build" --parallel 4
& .netrescue-tools\cmake\data\bin\ctest.exe --test-dir "$root\build" --output-on-failure
& "$root\build\NetRescue.exe"
```

### Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/NetRescue
```

On non-Windows platforms, `NetRescue` is the portable CLI demonstration. `build.bat` and `build.sh` are convenience scripts.

## Dashboard controls

- **Start / Pause / Resume** runs or pauses the configured ARQ transfer. **Run Traffic** starts a batch; while active it becomes **Stop Traffic**. `S` sends a batch and `Space` pauses/resumes.
- Select nodes, then press `1` to set the source or `2` to set the destination. The initial endpoints are the first and last nodes. Drag to reposition.
- Click a link then **Fail Link** or press `F` to fail/recover it. Select a node and press `N` to fail/recover it. `R` resets transfer metrics and restores failed elements; `F5` restores the demo network.
- Select a node and press `F2` to rename it or `T` to cycle its device type. Select a link to edit bandwidth, delay, loss, and cost by clicking the corresponding inspector row; `K` toggles directed/bidirectional mode. These edits are disabled during a live transfer.
- The left sidebar creates a node attached to the selected node. Press `L` with a selected node, then select a second node to add a link. `Delete` removes the selected link or node when no transfer is active.
- Click a topology name to generate an eight-node connected graph. Topology generation is disabled while a transfer is running or paused.
- Click protocol to cycle through Stop-and-Wait, GBN, and SR. Click the inspector rows to adjust window, loss, ACK loss, packet count, corruption, ACK corruption, timeout, and payload size (left side reduces; right side increases). `+/-` adjusts rate; `G` generates a seed. `[` and `]` also adjust payload size.
- Press `M` to toggle the educational congestion controller. A full forwarding queue drops that transmission, causing its normal timeout/NACK/retry recovery; the controller halves traffic rate, then increases it additively while the queue drains.
- Click the graph to cycle delivery ratio, throughput, latency, queue utilization, and packet loss. Press `V` to compare GBN and SR under the configured conditions and seed.
- `Ctrl+S` saves a topology; `Ctrl+O` loads one. `Esc` clears selection. `F` acts on the selected link; if none is selected it selects the first link.

The canvas depicts actual graph routes and in-flight ARQ data/ACK events. The event log and chart are sourced from simulation state, not generated presentation data.

## Modules and lab mapping

| Lab topic | Module |
|---|---|
| Graph/topology construction and templates | `network.c`, `topology.c` |
| Shortest-path routing and alternate routes | Dijkstra in `network.c` |
| Packet transmission and fault recovery | `network.c`, `arq.c` |
| Stop-and-Wait, GBN, Selective Repeat | `arq.c` |
| Checksum / CIDR analysis | `network.c`, `cidr.c` |
| Queue overflow and congestion indicators | `network.c` |
| Metrics and experiment history | `arq.c`, Win32 dashboard |
| Save/load | `persistence.c` |

Read [architecture](docs/architecture.md), [algorithms](docs/algorithms.md), [user manual](docs/user_manual.md), and [testing](docs/testing.md) for details.

## Limitations

The core uses fixed maximum capacities for predictable memory use. Congestion control is an educational additive-increase/multiplicative-decrease model over the route's bounded forwarding queue, not a full TCP congestion-control implementation. The current native UI is Windows-only; Linux and macOS builds run the CLI. CIDR is exposed as a C API rather than an interactive dashboard tool. Real TCP/UDP sockets and DNS are optional extensions and are not part of this simulator build.
