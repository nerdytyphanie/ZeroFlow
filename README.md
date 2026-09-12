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
- File and folder clipboard sharing with defaults of **384 MiB per selection and 128 regular files**, including nested and empty folders. Supervisors can raise either limit; 0 removes that user cap. File sizes and resume offsets use 64-bit values.
- TLS connections, with automatic acceptance and storage of peer fingerprints.
- Local-network ZeroFlow discovery and automatic placement of newly connected screens in the server layout.
- Optional JSON status/heartbeat output and a stdin command for graceful shutdown.

This fork does **not** provide a GUI, tray controls, graphical screen-layout editor, or macOS/Linux binaries. The original standalone ZeroFlow work is a separate project and is not distributed here.

## File clipboard

Copy a selection, then move the shared pointer to the receiving computer. Files transfer in the background over a separate TLS connection using 1 MiB buffers, with a 16 ms pause after every 64 MiB sent. Transfers support configurable speed, selection-size and file-count limits, progress reporting, and automatic retries. File contents are streamed directly to disk rather than loaded as a whole selection into memory. Input and text/image clipboard traffic retain their existing connection and limits.

The receiver stages the complete selection under a fresh `ZeroFlow-Clipboard-<id>` directory in the interactive user's ordinary Windows temp folder. Paste becomes available only after the entire selection arrives. Windows Explorer is asked to **move** the staged files to the paste destination; sender originals remain untouched. After all staged roots have been moved away, ZeroFlow clears that received file clipboard without clearing a newer copy. The paste acknowledgement also clears the sender's matching original clipboard selection and retires its transfer offer. A newer copy on either computer is preserved, including a fresh copy of the same source paths. Completion acknowledgements use the existing bounded connection retries and are safe to repeat. Applications that ignore Windows' preferred move action may copy instead, leaving the staged files and clipboard available. This completion handling applies to file/folder transfers, not general text or image pastes.

Unpasted successful transfers are left for normal temp cleanup; Windows does not guarantee a particular cleanup time. Failed or cancelled transfers remove their incomplete staging. Invalid paths, linked/reparse-point files, offline files, and selections exceeding the configured limits are rejected. Path metadata remains bounded to 3 MiB. A file-transfer failure does not close the keyboard/mouse connection.

The receiver reconnects up to three times after a file-channel failure, resuming at the first chunk that was not completely staged. TLS provides transport integrity, and a source whose identity, size or modification time changes during retry is rejected. Older senders are retried from the start with already-staged bytes discarded. Neither a retry nor a transfer limit restarts input sharing.

Tap **Insert twice within 500 ms** on the host keyboard to return the pointer and keyboard to the host, clear the screen lock, and end relative mouse capture. Key repeats do not trigger it; single Insert retains its normal behavior. Normal screen crossings remain available afterward.

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
- {"command":"transfer-limits","speedMiB":0,"selectionMiB":384,"files":128} applies saved supervisor limits without restarting input. Zero removes the corresponding user cap. A supervised peer advertises its policy to an unmanaged peer, so its controls apply in either direction. The default standalone policy is 384 MiB and 128 files.
- {"command":"screen-layout","request":"unique-request-id","lanes":{"up":[],"down":[],"left":[],"right":["handheld-1","handheld-2"]}} persists and applies a cardinal layout. Include every configured client exactly once, ordered outward from the host. Offline intermediate screens are skipped by the existing traversal. Layout updates replace only links and keep the live input hooks.
- Transfer records use type "transfer" with id, direction, state, bytes, total, bytesPerSecond, retries and file. States include preparing, transferring, retrying, staged, sent, ready, pasted, cancelled and failed. Ready means the receiving Windows clipboard was published, not just that bytes were sent.
- Heartbeats also retain sending/receiving snapshots, connectedClients, and the current layout plus its last save result. Transfer messages do not advance the event-loop heartbeat sequence.
- stderr contains connection-established messages, warnings, and failures. Routine screen crossings and successful heartbeat messages are not logged there.

Example status record:

```json
{"schema":1,"type":"status","role":"client","state":"connected","peers":0,"sequence":3,"uptimeMs":2010}
```

peers counts the server's attached clients. A client's connection is represented by state. sequence advances with each emitted status; uptimeMs is process uptime.

**Continuously drain both stdout and stderr.** Reading one line per timer tick can leave bursts queued; a full diagnostic pipe can block the input loop. A disconnected server does not by itself mean that the client process is unhealthy.

## Windows service mode

Use --service to run the executable as a LocalSystem Windows service named **ZeroFlow Embedded**, with --client, --host and an absolute --settings path in its service command line. Service installation requires administrator authority; ZeroFlow does not display an elevation prompt itself.

The service stays in Session 0 and starts a SYSTEM input worker in the active interactive session. The worker is the same executable. Stopping the service stops the worker; session changes move it to the active session. Automatic versus Manual startup is configured through Windows Service Control Manager.

A hidden helper running as the signed-in user captures Explorer's file clipboard and clears the matching selection after a remote paste. It communicates through private inherited pipes and uses a disposable executable copy in the user's temp directory. Keyboard and mouse input remain in the SYSTEM worker; file data uses the existing TLS transfer channel without an additional network port.

The service watches a private local named pipe. Advancing event-loop heartbeats refresh a ten-second deadline, with thirty seconds of startup/resume grace. Process exits are detected immediately; failures restart with backoff. A healthy client waiting for its server is not restarted. The worker is assigned to a job that closes it if its service exits.

## Build

The tested toolchain is Visual Studio 2022 (MSVC 14.44), CMake, and vcpkg at df25fb4f73c1c3bf7d019fc50742fc90902f8c60. The current dependency versions are Qt 6.11.1 and OpenSSL 3.6.4.

```powershell
vcpkg install "qtbase[core,network,thread,openssl]:x64-windows-static" "openssl:x64-windows-static" --host-triplet=x64-windows-static --classic
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF -DBUILD_INSTALLER=OFF
cmake --build build --config Release --target zeroflow-core
```

The CMake target name is zeroflow-core; its output is ZeroFlow.exe. Local validation tests are not distributed in this repository, source archives, or runtime.

## Attribution and license

ZeroFlow is a derivative of **Deskflow**, not an independent implementation of its input-sharing engine. Credit belongs to the Deskflow contributors and the Synergy/Barrier contributors whose work Deskflow builds on. Original copyright notices and source history are retained. This fork is not an official Deskflow release and is not endorsed by its maintainers.

The application code is licensed under **GPL-2.0-only with the OpenSSL linking exception**, with individual source-file notices retained. See [LICENSE](LICENSE), [LICENSES](LICENSES), and the combined LICENSE.txt supplied with releases. Qt Core/Network use their GPL-2.0 licensing option in this build; bundled dependencies retain their own notices.

Every binary release provides this fork's corresponding source, dependency source archives, and the build recipes. The single-file runtime packaging does not remove those source or notice obligations.
