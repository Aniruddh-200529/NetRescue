# User manual

The website is now the primary interface. See:

- [README.md](../README.md#using-the-website) for configuring topologies, introducing errors, running the protocols, comparison mode and exports;
- [teacher-demo.md](teacher-demo.md) for a guided walkthrough;
- [fault-injection.md](fault-injection.md) and [metrics.md](metrics.md) for the models behind the numbers.

The original Win32 dashboard (`NetRescue.exe`) is still built on Windows and uses the same C engine. Its
keyboard and mouse controls are unchanged: `S` start, `Space` pause/resume, `V` compare GBN and SR,
`F` fail the selected link, `1`/`2` set the source/destination, `Ctrl+S`/`Ctrl+O` save/load.
`NetRescueCLI.exe --preset <id>` runs any prebuilt experiment from the command line.
