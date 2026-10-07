# Testing and verification

## Automated core tests

The CTest `core` target covers node/link CRUD, duplicate/self-link rejection, directed graphs, Dijkstra reachability, inactive nodes/links, alternate-route packet recovery, packet loss/TTL/queue overflow, checksum, `/0`–`/32` CIDR boundaries, persistence success/failures, all topology templates, and ARQ behavior.

ARQ tests cover successful Stop-and-Wait, GBN window filling, successful Selective Repeat with history, data loss and retransmission, corruption/NACK recovery behavior, ACK loss and timeout, ACK corruption, zero-rate pause/stop, invalid endpoints, full loss, maximum packet/window bounds, the GBN/SR comparison API, link-failure rerouting during a transfer, bounded queue overflow, multiplicative rate reduction, and additive congestion recovery. Tests are compiled with assertions enabled in Release builds.

Run the full suite:

```powershell
ctest --test-dir build --output-on-failure --repeat until-fail:3
```

## Windows toolchain and verification

This workspace uses portable CMake 4.4.4, Ninja 1.13.2, and Zig 0.16.0 in `.netrescue-tools/`. The local MinGW compiler was avoided because its executable fails with “Illegal System DLL Relocation.” A hello-world C program was independently compiled, linked, and run with Zig before project builds.

Use the README's toolchain configuration command for a clean Ninja build, then run CTest and `NetRescueCLI.exe`. The Win32 dashboard is built with strict warning options and links only system UI libraries.

## Desktop verification boundary

The latest dashboard executable was launched in Windows. A Win32 message smoke pass selected a generated ring, assigned both endpoints, started and paused/resumed/stopped a transfer, cycled protocol and chart controls, reset the simulation, ran the GBN/SR comparison, and reset the demo; the process reported its window responsive afterward. The installed desktop-control provider exposed no native apps, so this used posted window input messages instead of an interactive visual automation session. Tests confirm the backing core operations, but the UI's rendered appearance, dialog presentation, and resulting on-screen values were not independently captured or asserted.
