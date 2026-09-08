# UniClipboard / ZeroFlow Feasibility Handoff

## Purpose

Preserve the complete discussion and relevant workspace context for evaluating whether ZeroFlow should replace its clipboard subsystem with UniClipboard while retaining ZeroFlow mouse and keyboard control.

## Repository

- Upstream: `https://github.com/uniclipboard/uniclipboard`
- Local clone: `D:\LOCAL-AI\Source\uniclipboard`
- The initial clone was not made with `--recurse-submodules`. UniClipboard's README says a source build requires its `iroh-blobs` submodule.
- ZeroFlow checkout: `D:\LOCAL-AI\Source\deskflow`

## User's requested target

- One and only one native general clipboard publication on each computer.
- One logical cross-computer clipboard owner/generation.
- Text, HTML, images, files, metadata, and paste-time requests use a dedicated second connection; the mouse/keyboard control connection must never clear, publish, or replace clipboard contents.
- Windows and macOS clipboard publication must not compete with another ZeroFlow/Deskflow clipboard path.
- Windows-to-macOS and macOS-to-Windows support for text, images, and files.
- Files transfer only when Paste actually requests bytes. Copy time may send only format, filename, size, and generation metadata.
- Windows file publication must retain FreeRDP's working persistent OLE behavior: `IDataObject`, `FileGroupDescriptorW`, indexed `FileContents` streams, message loop, and `OleIsCurrentClipboard` replacement test.
- macOS file publication must retain RemoteFilesVsPasteboard's working behavior: real empty placeholders, persistent manager, strongly retained `NSFilePresenter` objects, `relinquishPresentedItemToReader:`, and deliberate presenter cleanup only after paste completion or genuine clipboard replacement.
- The only file-provider adaptation is replacing the reference byte downloader/request callback with the selected transport.
- No prompts, UI interaction, account, space, pairing, passphrase, QR code, clipboard configuration, or application launch by the agent.
- Local-network operation only. Remove/disable relay fallback, NAT traversal, internet discovery, remote invitation services, and mobile compatibility networking.
- Retain/add ZeroFlow's mouse and keyboard capture, injection, screen layout, edge switching, keyboard mapping, and related control behavior.
- Images larger than the former approximately 3 MiB control-channel limit must be supported safely with bounded sizes, checked arithmetic, chunking/spooling, hash verification, and decoded-image limits.

## What the UniClipboard README claims

The README describes UniClipboard as a privacy-first cross-device clipboard application with first-class Windows, macOS, and Linux support. It claims:

- Bidirectional desktop clipboard synchronization.
- Text, image, and file support.
- Large-file streaming so files do not need to fit in memory.
- Encrypted P2P networking, NAT traversal, and relay fallback.
- Spaces, invitation codes, passphrases, device management, history, quick panel, CLI, and mobile compatibility.
- Active development with potentially unstable or missing features.

README-only conclusions:

- Replacing ZeroFlow's clipboard implementation entirely with UniClipboard could remove the competing-owner problem.
- Running both clipboard systems would preserve the conflict.
- "Streaming" does not prove that bytes transfer only when the user invokes Paste.
- The README alone does not prove FreeRDP-style OLE promises or macOS file presenters.
- Its default onboarding and networking conflict with ZeroFlow's no-configuration, LAN-only requirement.

## Brief source inspection performed

The user first requested README-only assessment, then authorized a brief source check. No full source deep dive has been performed.

Files/results inspected:

- `crates/uc-platform/src/clipboard/common.rs`
  - `CommonClipboardImpl::read_snapshot` enumerates and reads native clipboard representations.
  - Reads text, RTF, HTML, images, and file paths.
  - Normalizes files to `text/uri-list`.
  - Filters missing and zero-byte files.
  - Contains platform-specific handling and retries for declared but temporarily unreadable macOS data-provider formats.
  - Explicitly recognizes and skips Barrier's ownership marker as non-user clipboard data.
- `crates/uc-platform/src/clipboard/platform/mod.rs`
  - Uses platform-specific Windows and macOS clipboard implementations.
  - Windows/macOS watcher integration is based on `clipboard-rs` adapters.
- `crates/uc-platform/src/clipboard/format_id_mime.rs`
  - Maps native Windows/macOS identifiers to common MIME representations.
- Planning/debug material returned by the repository index
  - Documents resolved Windows/macOS image correctness, ownership, loopback, and performance defects.
  - Describes inbound file synchronization as completing/validating file transfer and then writing local file URIs to the system clipboard.
  - Mentions `ClipboardChangeOrigin::RemotePush` and remote snapshot hashes for preventing recapture loops.

One attempted direct read of `crates/uc-core/src/ports/clipboard_transport.rs` failed because that path was not present at the assumed checkout location. A later investigation should locate the actual workspace path rather than infer it.

### Confirmed file-transfer timing

A focused source check established that UniClipboard does **not** defer file bytes until Finder or Explorer invokes Paste.

Its dual-peer E2E test performs this sequence:

1. Create a 512 MiB file.
2. Put that file on the source system clipboard.
3. Capture the source clipboard.
4. Wait for the receiving peer to expose an active file receive.
5. Allow the user/test to cancel that receive from history.

No Paste operation occurs before the receiver starts downloading. The CLI implementation similarly publishes a file as a blob and keeps the source router alive while the receiving peer fetches it; the receiver fetch is initiated by inbound synchronization rather than a native paste request.

UniClipboard's current sequence is therefore:

`Copy -> announce -> automatically download/materialize -> publish destination-local file URI -> Paste`

It is not:

`Copy -> metadata offer -> native promised file -> Paste requests ranges -> transfer bytes`

This corrects the earlier visual impression that UniClipboard might wait for Paste.

## Current feasibility conclusion

Adapting UniClipboard is technically feasible and is probably cleaner than continuing to layer a file clipboard beside Deskflow's ordinary clipboard.

### Strong fit

- It is already organized around cross-platform clipboard capture, representation normalization, networking, remote application, and echo suppression.
- Text, HTML/RTF, and image synchronization are substantially closer to the requested result than ZeroFlow's current split control/file architecture.
- If ZeroFlow's clipboard subsystem is completely disabled, UniClipboard can be the sole process that watches and publishes the native clipboard.
- Local-only operation is an architectural simplification: retain direct peer transport and remove relay/NAT/internet/mobile compatibility paths.
- No-configuration behavior can replace spaces/invitations/passphrases with the existing ZeroFlow server/client identity and connection lifecycle.

### Not solved unchanged

- The inspected file workflow appears to synchronize files and then publish destination-local URIs. That is eager synchronization, even if the transfer itself is streamed.
- Eager transfer can make Paste work after completion but does not satisfy "send file bytes only when Paste happens."
- UniClipboard does not yet establish, from the inspected evidence, persistent Windows OLE promised files or retained macOS file presenters.
- Therefore its eager file completion/write-back stage must be replaced by the verbatim native providers:
  - Windows `IStream::Read` requests ranges through the chosen local transport.
  - macOS `relinquishPresentedItemToReader:` requests ranges and materializes the placeholder before allowing Finder to continue.
- Adding mouse/keyboard is not a trivial UI addition. The practical options are embedding ZeroFlow's existing control core as a library/service or porting its platform input implementations into the Rust/Tauri architecture. Embedding/reusing the existing core is likely less risky.
- Licensing and dependency implications have not yet been inspected.

### Non-negotiable large-file constraint

The user routinely copies 20–30 GiB files and the Mac has approximately 100 GiB free. Eager synchronization is not workable:

- It consumes network bandwidth even when Paste is never invoked.
- It can consume a substantial fraction of available Mac storage for transient clipboard state.
- Multiple clipboard generations, retries, or cached history entries can exhaust the destination disk.
- Waiting for complete materialization delays Paste availability and contradicts the required native promised-file behavior.

The accepted architecture must send only metadata at Copy time and transfer bytes only after a local paste consumer requests them. UniClipboard's eager file receiver and automatic materialization/cache path must be bypassed or replaced, not merely tuned.

For files:

- Copy sends generation, filename, size, attributes, and provider metadata only.
- Finder and Explorer expose Paste immediately from a persistent native promise.
- Windows `IStream::Read` and macOS `relinquishPresentedItemToReader:` initiate bounded transport range requests.
- Data is streamed into the paste placeholder/destination without first creating a second complete UniClipboard cache copy.
- Cancellation, clipboard replacement, disconnect, partial-file cleanup, and reconnect are generation-scoped.
- No prefetch, background synchronization, history materialization, or automatic blob download may transfer file contents before Paste.

UniClipboard may still be useful for text/image synchronization, format normalization, ownership tracking, and local-network transport. Its current file-transfer pipeline is disqualified for this use case unless replaced with true cross-platform paste-time lazy transfer.

## Required proposed architecture

1. UniClipboard becomes the sole native clipboard watcher and publisher.
2. Disable/remove all ZeroFlow/Deskflow clipboard behavior. No `CCLP`, `DCLP`, `EmptyClipboard`, `SetClipboardData`, `PasteboardClear`, or pasteboard write may originate from the mouse/keyboard control subsystem.
3. Use one logical clipboard generation with origin identity and explicit replacement.
4. Publish one atomic native clipboard generation containing all advertised formats.
5. Text and images use UniClipboard's normalized representations and local-only transport.
6. Files use UniClipboard's offer/routing layer but not its eager completion/write-back behavior.
7. Install the verbatim FreeRDP and RemoteFilesVsPasteboard provider lifetimes at the native platform boundary, adapting only byte requests to transport callbacks.
8. Actual file bytes cross only when Explorer/Finder requests them.
9. Remove spaces, pairing, passphrases, account/device-management onboarding, relay, NAT traversal, external discovery, and mobile compatibility services.
10. Attach clipboard setup automatically to ZeroFlow's existing LAN client/server relationship.
11. Reuse ZeroFlow's existing mouse/keyboard control core without allowing it to touch clipboard state.

## Image safety target

- Canonical cross-platform image representation should be PNG, with native conversion at platform boundaries.
- Use 64-bit payload lengths and checked arithmetic.
- Transfer bounded chunks and verify a content hash.
- Spool large encoded payloads rather than retaining unlimited network buffers.
- Validate dimensions, pixel format, row stride, and decoded byte count before publication.
- Proposed fixed zero-configuration ceilings discussed in-session:
  - 256 MiB encoded.
  - 512 MiB decoded pixel storage.
  - Dimensions also bounded by decoded storage.
- A compressed-size limit alone is insufficient because a small compressed image can decode into excessive memory.

## Relevant current ZeroFlow history

- Runtime file Paste still failed in both directions after numerous incremental repairs.
- Successful compilation, packaging, tests, and deployment were repeatedly and incorrectly described as fixes without runtime proof.
- Prior committed transport/provider work includes:
  - `502afbceb` `fix: publish native deferred file clipboards`
  - `64a7cefb9` `fix: use FreeRDP OLE clipboard publication`
  - `80be0595d` `fix: align promised file paste contracts`
  - `a04b64e49` `fix: avoid forced mac tray quit`
  - `ca4c17a68` `fix: harden deferred file paste transport`
  - `f56226257` `fix: preserve deferred clipboard ownership`
- The last successful requested release build produced:
  - `C:\Users\Typhanie\Desktop\ZeroFlow-1.27.0.436-win-x64.msi`
  - `/Applications/ZeroFlow Client.app`
  - `/Applications/ZeroFlow Server.app`
- All 25 Windows tests passed, and both macOS bundles built/deployed/code-sign verified, but runtime Paste still did not appear on either side.
- Neither deployed macOS app was launched by the agent. The running macOS client/core were later killed, and a macOS reboot was requested through System Events after passwordless `sudo` reboot failed.

## Relevant current ZeroFlow diagnosis

- The additive architecture was fundamentally wrong: ordinary clipboard handling and the file clipboard could each publish/clear the same native OS clipboard.
- There is only one native general clipboard on each machine. The second connection must be transport, not a second clipboard.
- FreeRDP keeps the hidden window, `IDataObject`, descriptors, streams, protocol state, and ownership test in one persistent `wfClipboard` lifetime.
- RemoteFilesVsPasteboard keeps presenters strongly retained by a persistent manager and removes them deliberately, not whenever a temporary clipboard wrapper is destroyed.
- ZeroFlow copied pieces of these APIs but distributed lifetime and cleanup across temporary platform clipboard wrappers, client/server routing, transfer coordinators, and global deferred state.
- A concrete macOS contradiction was found: `OSXClipboard::~OSXClipboard()` globally canceled deferred transfers and cleared presenters, unlike the reference's persistent manager.

## Current uncommitted ZeroFlow work

Before UniClipboard was proposed, an initial unified-second-stream conversion was started in the ZeroFlow checkout. It is incomplete, unbuilt, unvalidated, and must not be mistaken for a finished solution.

Changes include:

- `ZEROFLOW_PASTE_RECOVERY_PLAN.md` expanded with unified-owner requirements.
- Added `src/lib/deskflow/UnifiedClipboard.h` for generation envelopes.
- Added `ZCLP` dedicated-stream clipboard messages.
- Began routing ordinary text/HTML/bitmap payloads through the dedicated stream.
- Disabled active legacy control-stream clipboard sends and made legacy control clipboard messages discard rather than publish.
- Changed clipboard propagation to be independent of screen focus.
- Began making macOS presenter/publication identity process-scoped rather than temporary-instance-scoped.
- Removed global deferred cancellation/presenter cleanup from temporary `OSXClipboard` destruction.
- Continued partial FreeRDP COM behavior alignment.
- Replaced the former small configured payload check on the new path with a preliminary fixed 256 MiB encoded limit.

This work does not yet provide lazy text/images, complete decoded-image validation, a complete verbatim provider port, or runtime proof. If UniClipboard is selected as the new foundation, decide explicitly whether to preserve, revert, or supersede these uncommitted ZeroFlow edits without disturbing unrelated translation files or `.deps`.

## Protected unrelated ZeroFlow workspace state

Do not stage, revert, delete, or alter these unless separately authorized:

- `translations/deskflow_es.ts`
- `translations/deskflow_it.ts`
- `translations/deskflow_ja.ts`
- `translations/deskflow_ko.ts`
- `translations/deskflow_ru.ts`
- `translations/deskflow_zh_CN.ts`
- `.deps/`

`ZEROFLOW_PASTE_RECOVERY_PLAN.md` was untracked when first edited and was backed up before modification at:

`C:\Users\Typhanie\.opencode-backups\deskflow\ZEROFLOW_PASTE_RECOVERY_PLAN.md.bak-20260829-205057`

## Focused investigation required after compaction

1. Confirm UniClipboard's license and whether its source can be incorporated/distributed with ZeroFlow.
2. Locate its actual Cargo workspace roots and initialize required submodules only if explicitly authorized.
3. Trace one complete text/image path: watcher → snapshot → transport → echo suppression → native write.
4. Trace one complete file path and establish exactly when bytes transfer.
5. Identify every relay, NAT traversal, discovery, invitation, account/space, mobile HTTP, and external-service component that must be removed or bypassed.
6. Identify the smallest direct-LAN transport boundary suitable for automatic ZeroFlow sessions.
7. Determine how to host the existing ZeroFlow mouse/keyboard core without linking clipboard behavior back in.
8. Identify Windows/macOS native extension points for the verbatim promised-file providers.
9. Determine whether UniClipboard can advertise a file offer without eagerly capturing or persisting file bytes.
10. Produce a concrete migration plan, file list, and validation matrix before implementation.
11. Search for an existing maintained implementation that already provides true paste-triggered lazy file transfer across Windows and macOS, rather than another eager clipboard-sync/cache application.
12. Verify candidate behavior from source or tests: copying a multi-gigabyte file must transfer metadata only, and network byte transfer must begin only after Explorer/Finder invokes Paste.

## Acceptance criteria

- One native clipboard publisher per machine.
- ZeroFlow control code cannot clear or publish clipboard content.
- Windows↔macOS text paste works in both directions.
- Windows↔macOS image paste works in both directions above 3 MiB within fixed safety limits.
- Windows↔macOS file Paste is visible immediately after metadata arrives.
- No file bytes transfer before a paste consumer requests them.
- Explorer retains `FileGroupDescriptorW` and `FileContents` until genuine replacement.
- Finder retains working placeholder URLs and presenters until genuine replacement or successful materialization.
- Genuine local copies replace the generation exactly once without feedback loops.
- LAN-only operation performs no relay, NAT traversal, internet discovery, invitation-service, or mobile compatibility traffic.
- No clipboard setup, pairing, passphrase, prompt, or UI interaction is required.
- Mouse and keyboard control retains existing ZeroFlow behavior while remaining clipboard-independent.
