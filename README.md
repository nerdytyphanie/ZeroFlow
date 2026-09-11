# ZeroFlow

ZeroFlow is a **Windows-only, fully headless** fork of [Deskflow](https://github.com/deskflow/deskflow). It shares a computer's keyboard, mouse, and clipboard with another computer on a trusted local network.

It is intended for command-line use and integration into applications that manage its settings and lifetime. **There is no graphical interface, tray icon, setup wizard, or standalone desktop application.** Running the executable without the required arguments does not open a window.

## Download

Get **ZeroFlow.exe** and **LICENSE.txt** from the [releases page](https://github.com/nerdytyphanie/ZeroFlow/releases). Keep the license beside the executable.

The portable Windows x64 runtime is one executable, approximately 15 MB. It does not require adjacent Qt, OpenSSL, or Visual C++ runtime DLLs. Source and dependency source archives are separate release assets for people who want to rebuild it; they are not needed to run it.

## What this fork supports

- Headless server and client modes in the same executable.
- Keyboard and mouse sharing between screens.
- Text and image clipboard sharing over the main connection, limited to 3 MiB by default. Oversized clipboard transfers are discarded without disconnecting input.
- File and folder clipboard sharing, limited to **384 MiB total per selection and 128 regular files**, including nested folders and empty folders. Any combination within those limits is supported.
- TLS connections, with automatic acceptance and storage of peer fingerprints.
- Local-network ZeroFlow discovery and automatic placement of newly connected screens in the server layout.
- Optional JSON status/heartbeat output and a stdin command for graceful shutdown.

This fork does **not** provide a GUI, tray controls, graphical screen-layout editor, or macOS/Linux binaries. The original standalone ZeroFlow work is a separate project and is not distributed here.

## File clipboard

Version 1.2 adds file and folder sharing. Update both computers to use it. Copy a selection, then move the shared pointer to the receiving computer. Files transfer in the background over a separate TLS connection using 1 MiB buffers, with a 16 ms pause after every 64 MiB sent. File contents are streamed directly to disk rather than loaded as a whole selection into memory. Input and text/image clipboard traffic retain their existing connection and limits.

The receiver stages the complete selection under a fresh `ZeroFlow-Clipboard-<id>` directory in the interactive user's ordinary Windows temp folder. Paste becomes available only after the entire selection arrives. Windows Explorer is asked to **move** the staged files to the paste destination; sender originals remain untouched. After all staged roots have been moved away, ZeroFlow clears that received file clipboard without clearing a newer copy. Applications that ignore Windows' preferred move action may copy instead, leaving the staged files and clipboard available.

Unpasted successful transfers are left for normal temp cleanup; Windows does not guarantee a particular cleanup time. Failed or cancelled transfers remove their incomplete staging. Invalid paths, linked/reparse-point files, offline files, oversized selections, and selections exceeding 128 regular files are rejected. A file-transfer failure does not close the keyboard/mouse connection.

Automatic trust is intended for a trusted local network. It is not an authenticated public-internet pairing workflow. An upstream Deskflow peer still uses its own certificate approval and screen-layout configuration. The connection protocol is based on Deskflow; interoperability with a current stock Deskflow build still needs a dedicated test.

## Run

Use a separate, absolute settings path for each computer:

```powershell
.\ZeroFlow.exe --server --settings C:\ZeroFlow\server\settings.ini
.\ZeroFlow.exe --client --host 192.168.0.10 --settings C:\ZeroFlow\client\settings.ini
```

Replace the example address with the server's local IP. Omit --host to use the saved host, or discover a ZeroFlow server if no host is saved. Identity, trusted fingerprints, and layout files are kept beside the selected settings file. --name sets the screen name; --port changes the sharing port.

Default network ports:

| Port | Purpose |
| --- | --- |
| TCP 24800 | Keyboard, mouse, and clipboard connection |
| UDP 24801 | ZeroFlow local discovery |
| TCP 24802 | Background file/folder clipboard transfer |

Allow these only on the local network. Only one sharing core can own an interactive Windows session at a time.

## Integrate with another application

Start the executable without a console window and redirect its standard handles:

- --status-json emits newline-delimited JSON on stdout. A heartbeat is produced every two seconds by the input event loop.
- --control-stdin accepts {"command":"stop"} followed by a newline. Closing stdin also requests shutdown.
- stderr contains connection-established messages, warnings, and failures. Routine screen crossings and successful heartbeat messages are not logged there.

Example status record:

```json
{"schema":1,"type":"status","role":"client","state":"connected","peers":0,"sequence":3,"uptimeMs":2010}
```

peers counts the server's attached clients. A client's connection is represented by state. sequence advances with each emitted status; uptimeMs is process uptime.

**Continuously drain both stdout and stderr.** Reading one line per timer tick can leave bursts queued; a full diagnostic pipe can block the input loop. A disconnected server does not by itself mean that the client process is unhealthy.

## Windows service mode

Version 1.1 adds --service to the same executable. Install it as a LocalSystem Windows service named **ZeroFlow Embedded**, with --client, --host and an absolute --settings path in its service command line. Service installation requires administrator authority; ZeroFlow does not display an elevation prompt itself.

The service stays in Session 0 and starts a SYSTEM input worker in the active interactive session. The worker is the same executable. Stopping the service stops the worker; session changes move it to the active session. Automatic versus Manual startup is configured through Windows Service Control Manager.

The service watches a private local named pipe. Advancing event-loop heartbeats refresh a ten-second deadline, with thirty seconds of startup/resume grace. Process exits are detected immediately; failures restart with backoff. A healthy client waiting for its server is not restarted. The worker is assigned to a job that closes it if its service exits.

Release 1.0.0 supports only direct process operation. Use 1.1 or later for service mode.

## Build

The tested toolchain is Visual Studio 2022 (MSVC 14.44), CMake, and vcpkg at df25fb4f73c1c3bf7d019fc50742fc90902f8c60. The current dependency versions are Qt 6.11.1 and OpenSSL 3.6.4.

```powershell
vcpkg install "qtbase[core,network,thread,openssl]:x64-windows-static" "openssl:x64-windows-static" --host-triplet=x64-windows-static --classic
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF -DBUILD_INSTALLER=OFF
cmake --build build --config Release --target zeroflow-core
```

The historical CMake target name is zeroflow-core; its output is ZeroFlow.exe. File clipboard tests are included in `tests/clipboard-files`, separately from the runtime. Configure that directory with the same generator, static vcpkg toolchain and triplets, then build and run `ctest -C Release --output-on-failure` in its build directory.

Two-machine testing has confirmed repeated keyboard/mouse crossings between a desktop and an MSI Claw, with no unwanted clicks or disconnects in the final test. Secure-desktop behavior and stock Deskflow interoperability require their own validation.

Version 1.2 was also tested with clipboard transfers from an MSI Claw to a desktop: 128 files totaling 371.2 MiB staged in approximately 7 seconds, and one 383 MiB file in approximately 4.1 seconds. SHA-256 hashes matched before and after paste, staged files moved out of temp, and clipboard clearing was observed. Input heartbeats continued and the user reported no loss of control while crossing screens during the folder transfer. These timings describe that network, not a guaranteed transfer rate.

## Attribution and license

ZeroFlow is a derivative of **Deskflow**, not an independent implementation of its input-sharing engine. Credit belongs to the Deskflow contributors and the Synergy/Barrier contributors whose work Deskflow builds on. Original copyright notices and source history are retained. This fork is not an official Deskflow release and is not endorsed by its maintainers.

The application code is licensed under **GPL-2.0-only with the OpenSSL linking exception**, with individual source-file notices retained. See [LICENSE](LICENSE), [LICENSES](LICENSES), and the combined LICENSE.txt supplied with releases. Qt Core/Network use their GPL-2.0 licensing option in this build; bundled dependencies retain their own notices.

Every binary release provides this fork's corresponding source, dependency source archives, and the build recipes. The single-file runtime packaging does not remove those source or notice obligations.
