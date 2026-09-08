# ZeroFlow

A Windows-only, headless fork of ZeroFlow/Deskflow for embedding keyboard,
mouse and clipboard sharing into another application. The portable build is
one `ZeroFlow.exe`, without Qt DLLs, a tray icon or a console window.
The original ZeroFlow standalone applications remain a separate project.

## Run

```powershell
.\ZeroFlow.exe --server --settings C:\ZeroFlow\server\settings.ini
.\ZeroFlow.exe --client --host 192.168.0.10 --settings C:\ZeroFlow\client\settings.ini
```

Use a separate, absolute settings path for each instance. Identity, trust and
screen-layout files stay beside that settings file. Omit `--host` to discover
a ZeroFlow server when no host has been saved. TCP 24800 carries sharing;
UDP 24801 carries local-network discovery. `--port` overrides the sharing port.

Connections use Deskflow's TLS protocol. This fork automatically accepts TLS
peer fingerprints and assigns a free screen edge to new clients; use it only
on trusted local networks. An upstream Deskflow peer must still approve our
certificate and configure its own server layout as appropriate. Stock
Deskflow interoperability has not yet been validated with this build.

Text and image clipboard data use the normal Deskflow connection. The former
secondary file-transfer connection and file-copy extensions are not included.

## Process integration

Add `--status-json` and redirect stdout to receive schema-1 NDJSON records:

```json
{"schema":1,"type":"status","role":"client","state":"connected","peers":0,"sequence":3,"uptimeMs":2010}
```

`peers` counts clients attached to a server; client connectivity is represented
by `state`. A heartbeat is emitted every two seconds by the active event loop.
The output writer retains at most one pending snapshot if its reader stalls.
Human-readable diagnostics go to stderr: a connection-established message,
warnings for failed operations, and errors/fatal failures. Routine screen
crossings, successful status updates, and debug output are suppressed.
Heartbeats are not written to disk and are emitted only with `--status-json`.
When redirecting output, continuously drain both stdout and stderr. A full
stderr pipe blocks the synchronous diagnostic writer and can stall input and
protocol keep-alives. The example test launcher drains every available line.

Add `--control-stdin` and keep stdin open to own the process lifetime. Send
`{"command":"stop"}` followed by a newline to stop cleanly; closing stdin
also stops it. Without this option, parent exit does not stop the core.

Only one sharing core can own an interactive session at a time. This build
does not yet install a Windows service; interactive-session SYSTEM service
support is the next separate implementation step.

## Build

Tested with Visual Studio 2022 (MSVC 14.44), CMake and vcpkg commit
`df25fb4f73c1c3bf7d019fc50742fc90902f8c60` (Qt 6.11.1 / OpenSSL 3.6.4).

```powershell
vcpkg install "qtbase[core,network,thread,openssl]:x64-windows-static" "openssl:x64-windows-static" --host-triplet=x64-windows-static --classic
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF -DBUILD_INSTALLER=OFF
cmake --build build --config Release --target zeroflow-core
python tests/headless_smoke.py build/bin/Release/ZeroFlow.exe
```

For the separate clipboard regression executable, configure with
`-DBUILD_HEADLESS_TESTS=ON`, build target `clipboard-limit-test`, and run it.
It sends an 18 MB clipboard through the real protocol parser, verifies that
it is discarded without an error or retained payload, then verifies that a
normal transfer still succeeds on the same connection. This test target has
no install rule and is disabled by default.

Release builds use static Qt Core/Network, OpenSSL and the MSVC runtime.
The production target does not compile or install the scripts under `tests`.
The PNG icon source and multi-resolution Windows ICO are in `src/apps/res`.

## Validation and distribution

Automated tests cover a loose EXE without adjacent DLLs, argument validation,
heartbeat state/sequence, duplicate-session protection, persistent TLS identity,
stdin stop and stdin EOF. Two-machine testing on September 8, 2026 confirmed
TLS connection, automatic screen placement, and repeated mouse/keyboard
crossings between a desktop and an MSI Claw. The user confirmed that the
unwanted clicks and disconnects were gone; both sides retained connected
heartbeats throughout the final run. Both test processes were stopped afterward.
Secure-desktop operation and upstream interoperability still require their
respective device tests.

Retain the upstream license notices and OpenSSL exception. Distribute the
license beside the executable and provide corresponding source. The release includes dependency source archives and build recipes alongside
the executable. Qt Core/Network are used under their GPL-2.0 option; retain
the bundled third-party notices. Download `LICENSE.txt` beside `ZeroFlow.exe`.
The executable alone is sufficient to run; the source archive is for rebuilding.
