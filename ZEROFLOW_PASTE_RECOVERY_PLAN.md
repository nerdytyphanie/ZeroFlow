# ZeroFlow Cross-Platform File Paste Recovery Plan

## Objective

Make file copy/paste work transparently in both directions between Windows and macOS. File metadata and paste-time range traffic must use the dedicated second TCP stream. Windows Explorer and macOS Finder must expose their normal Paste command immediately after a remote file copy. File contents must remain lazy, with no configuration required on either endpoint.

The dedicated second stream is the sole transport for every clipboard format. There is exactly one native general clipboard publication on each machine and one logical clipboard owner/generation across the connection. The normal control stream must never carry, publish, clear, or replace clipboard text, HTML, bitmap, file metadata, or file contents.

## User constraints

- Port the known-working local reference implementations rather than approximating them:
  - `D:\LOCAL-AI\Source\FreeRDP\client\Windows\wf_cliprdr.c`
  - `D:\LOCAL-AI\Source\RemoteFilesVsPasteboard\Source\`
- Changes from those implementations are allowed only where required by C++/Objective-C++ integration and ZeroFlow's dedicated file transport.
- The second pipe carries capability negotiation, manifests, range requests, range responses, and file bytes.
- Clipboard publication must be invisible to the user and must not eagerly transfer entire files.
- Do not launch the applications. The user performs runtime validation.
- The latest instruction prohibited a build after the final ownership-loop repair.
- Preserve the working native file-clipboard implementations verbatim in behavior and lifetime:
  - Windows uses FreeRDP's persistent OLE owner, `IDataObject`, `FileGroupDescriptorW`, indexed `FileContents` streams, message loop, and `OleIsCurrentClipboard` replacement test.
  - macOS uses RemoteFilesVsPasteboard's persistent manager, real empty placeholders, strongly retained `NSFilePresenter` objects, `relinquishPresentedItemToReader:`, and deliberate presenter removal only after paste completion or genuine clipboard replacement.
- The only file-provider adaptation is replacing the reference downloader/RDP callbacks with ZeroFlow's existing dedicated transport requests and responses.
- Do not add UI, prompts, configuration, eager file transfer, or user interaction.
- Temporary clipboard wrappers must never own or cancel the active native clipboard provider.

## Authoritative unified clipboard design

- The second TCP stream carries a generation-scoped clipboard offer, format requests, range requests, responses, acknowledgements, replacement, and cancellation.
- Every genuine local Copy creates one generation with an origin endpoint and advertised formats.
- Remote native publications are proxies for that same generation; their native notifications are echoes and must never create another generation.
- Only a genuine new local Copy retires the current generation.
- Screen switching does not own, trigger, or limit clipboard delivery.
- The platform publishes all formats for a generation atomically through one persistent native owner:
  - UTF-8 text.
  - UTF-8 HTML.
  - PNG as the canonical image wire representation, with native Windows DIB/BMP and macOS image conversion retained at the platform boundary.
  - File names, sizes, attributes, and lazy file-content providers.
- Text and images may also be delayed-rendered. Only the offer and format metadata cross at Copy time; payload bytes cross when a local consumer requests the format.
- File bytes always remain lazy:
  - Explorer `IStream::Read` issues second-pipe range requests.
  - Finder `relinquishPresentedItemToReader:` issues second-pipe range requests before allowing the reader to continue.

## Bitmap safety beyond the former 3 MiB control limit

- The control-stream clipboard limit does not apply to the second stream.
- Use 64-bit payload sizes and checked arithmetic.
- Transfer bounded chunks and hash the completed payload.
- Spool large encoded images rather than accumulating unbounded network data.
- Validate encoded size, dimensions, pixel format, row stride, and decoded byte count before native publication.
- Use fixed non-configurable safety ceilings so the feature remains zero-configuration; initial ceilings are 256 MiB encoded and 512 MiB decoded, with dimensions additionally bounded by decoded storage.
- Reject malformed, oversized, truncated, generation-mismatched, or hash-mismatched image data without disturbing the current native clipboard generation.

## Control-stream removal requirement

- Remove clipboard payload transmission through `kMsgCClipboard`, `kMsgDClipboard`, and clipboard `StreamChunker` paths for ZeroFlow peers.
- Move text, HTML, bitmap, files, and all clipboard ownership messages to the dedicated second stream.
- The control connection remains responsible only for input, screen, connection, and non-clipboard protocol traffic.
- No control-path callback may invoke `EmptyClipboard`, `SetClipboardData`, `PasteboardClear`, `clearContents`, or a platform clipboard `setClipboard` after the unified generation is published.
- Compatibility fallback must not be allowed to write a second native clipboard in ZeroFlow mode.

## Completed commits before the latest repair

- `502afbceb` — `fix: publish native deferred file clipboards`
- `64a7cefb9` — `fix: use FreeRDP OLE clipboard publication`
- `80be0595d` — `fix: align promised file paste contracts`
- `a04b64e49` — `fix: avoid forced mac tray quit`
- `ca4c17a68` — `fix: harden deferred file paste transport`

## Implemented transport and platform work

### Dedicated second pipe

- Client opens a second `PacketStreamFilter` connection after the main handshake.
- The second connection uses the `ZFile01` hello protocol and attaches to the matching server-side client by name and remote address.
- Capability `ZFCP4-LAZY-RANGE-CAP`, manifests, range requests, range responses, and file data use only the file stream.
- Added automatic client-side file-stream reconnect after connection failure, malformed hello, version mismatch, stream shutdown, socket disconnect, or output error.
- Client-to-server file clipboard metadata is queued until file capability negotiation completes.
- Server-to-client file clipboard metadata is queued until capability negotiation completes.
- Server now detaches and deletes dead file streams, clears stale capability ownership, and accepts replacement file streams.
- Pending target clipboard state is preserved across target-side file-stream reconnects.
- Ordinary clipboard changes clear stale queued file transfers.
- Range waits were reduced from five minutes to 30 seconds.

### Windows promised-file implementation

- Uses an OLE STA with a message-only window and persistent message pump.
- Publishes `IDataObject` through `OleSetClipboard`.
- Exposes `CFSTR_FILEDESCRIPTORW` and indexed `CFSTR_FILECONTENTS`/`IStream` entries.
- Retains one stream per promised file and reuses it for indexed content requests, matching FreeRDP's lifecycle.
- Added a COM `IEnumFORMATETC` implementation with clone/reset/skip/next behavior.
- File descriptors expose attributes, size, progress UI, and the remote filename.
- `IStream::Read` requests bytes lazily through `DeferredFileTransfer` and fulfills the consumer's complete requested read without an artificial 4 MiB subdivision.
- Clipboard-open retries now match FreeRDP's 10 attempts with 10 ms delays.

### macOS deferred-file implementation

- Creates/verifies parent directories and real empty placeholder files before publishing URLs.
- Registers a retained `NSFilePresenter` for each deferred file.
- Publishes existing local placeholder URLs through `NSPasteboard`.
- `relinquishPresentedItemToReader:` fetches data over the second pipe, writes the placeholder, invokes Finder's reader completion, and removes the presenter.
- Sequential macOS range requests are 64 MiB to avoid a 4 MiB latency bottleneck while avoiding multi-gigabyte allocations.

## Build and deployment evidence for commit `ca4c17a68`

### Windows

- Configured with MSVC 14.44, Qt 6.10.3, Ninja, and the local vcpkg release triplet.
- Initial package attempt failed because CTest could not find runtime DLLs (`0xc0000135`). This was an environment failure, not a compile failure.
- Re-running with Qt, vcpkg, and build `bin` directories on `PATH` succeeded.
- All 25 Windows tests passed.
- Installer produced and staged at:
  - `C:\Users\Typhanie\Desktop\ZeroFlow-1.27.0.436-win-x64.msi`

### macOS

- Committed source was transferred with `git archive HEAD` to `mac-build:$HOME/ZeroFlow`.
- Both targets built successfully:
  - `zeroflow-client`
  - `zeroflow-server`
- Both bundles were processed with `macdeployqt`, ad-hoc signed, copied, and strictly verified at:
  - `/Applications/ZeroFlow Client.app`
  - `/Applications/ZeroFlow Server.app`
- Neither app was launched by the agent.

## Runtime evidence after deployment

- Windows server process and macOS client were running.
- Windows had two established TCP connections from the Mac to port 24800, proving that both the normal control stream and dedicated file stream were connected.
- Despite both streams being established, Explorer did not expose Paste.
- Native Windows clipboard enumeration showed only `Deskflow Ownership`; `FileGroupDescriptorW` and `FileContents` had disappeared.

## Exact root cause found

The promised-file clipboard was being published successfully and then immediately destroyed by ZeroFlow's own clipboard ownership feedback path:

1. `OleSetClipboard` published the promised-file `IDataObject`.
2. Commit `80be0595d` removed the private `Deskflow Ownership` format from that object to imitate FreeRDP.
3. `MSWindowsClipboard::has(Files)` recognizes only `CF_HDROP`, not virtual-file `FileGroupDescriptorW`/`FileContents` formats.
4. The Windows clipboard observer therefore classified ZeroFlow's own OLE publication as a new external, non-file clipboard.
5. `ServerProxy::onClipboardChanged()` canceled `DeferredFileTransfer`, cleared the lazy-transfer state, and sent the clipboard through the ordinary path.
6. That ordinary clipboard write replaced the promised-file object with only `Deskflow Ownership`, disabling Explorer's Paste command.

The earlier claim that FreeRDP had been ported verbatim was incorrect: the COM surface had been copied, but FreeRDP's `OleIsCurrentClipboard(data_obj)` ownership lifecycle had not been integrated.

## Latest uncommitted repair — do not lose

The final repair is currently uncommitted and was intentionally not built because the user explicitly said not to build.

Subsequent unified-owner work has begun. The macOS provider retention is being moved out of temporary `OSXClipboard` instance lifetime: temporary destruction no longer globally cancels deferred transfers or unregisters presenters, and the publication identity is process-scoped rather than instance-scoped. This is not yet a completed port or runtime-validated result.

The initial control-stream extraction is also implemented but not yet built or runtime validated:

- Added `ZCLP` unified clipboard envelopes on the dedicated stream with a 64-bit generation ID.
- Client-originated ordinary clipboard payloads no longer emit `CCLP` or `DCLP`; they queue until the dedicated stream is attached and capability negotiation completes.
- Server-to-client ordinary clipboard payloads use the dedicated stream and are queued across file-stream absence.
- Legacy control-stream clipboard ownership and payload messages are consumed and discarded without touching the native clipboard.
- Server clipboard-grab handling no longer tells other screens to empty/take the native clipboard through the control connection.
- Clipboard capture is immediate and independent of screen focus.
- The former configured small control-channel payload check has been replaced on the new path by a fixed 256 MiB encoded ceiling; decoded-image validation and lazy text/image format requests remain to be implemented before this work is complete.

### Windows exact ownership repair

Files:

- `src/lib/platform/MSWindowsClipboard.cpp`

Changes:

- Added `kOleIsCurrentClipboard` handling to the OLE publisher's message-only STA window.
- Added `DeferredClipboardPublisher::isCurrent()`.
- `isCurrent()` synchronously asks the publisher's OLE STA to execute `OleIsCurrentClipboard(m_dataObject)`.
- `MSWindowsClipboard::isOwnedByDeskflow()` now returns true when either:
  - the ordinary private ownership format exists, or
  - FreeRDP's exact `OleIsCurrentClipboard(dataObject)` test says the promised-file object is still current.
- This prevents ZeroFlow's Windows clipboard observer from feeding its own promised-file publication back through the ordinary clipboard path while still detecting a real external clipboard replacement.

### macOS exact self-notification repair

Files:

- `src/lib/platform/OSXClipboard.h`
- `src/lib/platform/OSXClipboard.cpp`

Changes:

- Added `m_deferredChangeCount`.
- After successful deferred URL publication, ZeroFlow stores the exact `NSPasteboard.changeCount`.
- `OSXClipboard::synchronize()` suppresses the notification when the current change count equals ZeroFlow's own deferred publication.
- A different change count is treated as an actual external clipboard change.
- This prevents the macOS clipboard observer from canceling the deferred transfer because of its own pasteboard write.

## Immediate next actions

1. Review the three latest uncommitted platform-file changes without altering unrelated translation files or `.deps`.
2. Commit only:
   - `src/lib/platform/MSWindowsClipboard.cpp`
   - `src/lib/platform/OSXClipboard.cpp`
   - `src/lib/platform/OSXClipboard.h`
   when the user explicitly requests another commit.
3. Do not build while the user's no-build instruction remains active.
4. When the user authorizes rebuilding, follow `ZEROFLOW_BUILD.md` exactly, including the corrected Windows test `PATH` containing Qt, vcpkg release DLLs, and the build `bin` directory.
5. Do not launch either application; deploy only when explicitly requested.
6. Runtime acceptance must verify both directions:
   - Windows copy → macOS Finder immediately enables Paste.
   - macOS copy → Windows Explorer immediately enables Paste.
   - Windows clipboard retains `FileGroupDescriptorW` and `FileContents` after publication.
   - Pasting fetches complete bytes over the second connection.
   - Copying a different local clipboard item cancels/replaces the deferred transfer.
   - Disconnecting and reconnecting the file stream does not destroy the visible promised-file clipboard.

## Protected unrelated workspace state

Do not stage, revert, delete, or otherwise alter unrelated generated/current workspace entries unless separately authorized:

- `translations/deskflow_es.ts`
- `translations/deskflow_it.ts`
- `translations/deskflow_ja.ts`
- `translations/deskflow_ko.ts`
- `translations/deskflow_ru.ts`
- `translations/deskflow_zh_CN.ts`
- `.deps/`
