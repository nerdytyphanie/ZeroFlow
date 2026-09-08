# Clipboard Publication Review

## Reported failure

Cross-device lazy clipboard file transfer is fully broken: no Paste affordance appears on either the Windows or macOS side of a transfer.

## External review recommendations

### 1. macOS pasteboard publication

In `src/lib/platform/OSXClipboard.cpp`, `OSXClipboard::add()` mixed the legacy
`declareTypes:owner:` API with modern `writeObjects:` publication. The review
recommended removing:

```objc
[pasteboard declareTypes:@[ NSURLPboardType ] owner:nil];
```

It also identified a redundant double start of a new pasteboard session:

```objc
[pasteboard clearContents];
[pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
```

The recommended sequence is to retain only
`prepareForNewContentsWithOptions:` and then call `writeObjects:` so the URL
objects perform their own pasteboard type negotiation.

### 2. Windows OLE publication

In `src/lib/platform/MSWindowsClipboard.cpp`, `MSWindowsClipboard::close()`
closed the Win32 clipboard and then called `DeferredClipboardPublisher::publish()`.
That method used `PostMessageW`, so it returned before the OLE STA executed
`OleSetClipboard()`.

The review recommended making publication synchronous: block until the OLE
thread confirms whether `OleSetClipboard()` succeeded. It also recommended
checking the relationship between the private ownership marker written by
`empty()` and the later OLE publication.

### 3. Deferred-path diagnostics

The review suggested temporary logging in `DeferredFileTransfer::add()` and
`DeferredFileTransfer::contains()`, together with both platform `add()` call
sites, to compare the exact placeholder paths registered with the exact paths
presented to the native clipboard. Path round-tripping through
`encodePaths`, `splitFilePaths`, and `u8path` remains a plausible secondary
failure point.

The proposed order was: fix items 1 and 2, rebuild, and retest Windows-to-macOS
and macOS-to-Windows lazy file Paste before adding the item 3 diagnostics.

## Technical analysis

### macOS assessment

The recommendation is valid and likely addresses a native publication defect.
The previous sequence combined three different publication operations:

```objc
[pasteboard clearContents];
[pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
[pasteboard declareTypes:@[ NSURLPboardType ] owner:nil];
[pasteboard writeObjects:urls];
```

`prepareForNewContentsWithOptions:` already begins new pasteboard contents.
Calling `clearContents` first is redundant. More importantly,
`declareTypes:owner:` establishes a legacy declared-type ownership contract,
while `writeObjects:` expects each `NSPasteboardWriting` object to advertise
and provide its own representations. Mixing those contracts can interfere with
the file-URL representations Finder uses to decide whether Paste is available.

The corrected sequence is:

```objc
[pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
BOOL success = [pasteboard writeObjects:urls];
```

### Windows assessment

The asynchronous publication is a concrete race:

1. `close()` calls `CloseClipboard()`.
2. `publish()` queues `kOleSetClipboard` with `PostMessageW`.
3. `close()` returns before `OleSetClipboard()` executes.
4. A clipboard observer can inspect the clipboard during that gap.
5. `isCurrent()` can execute before the posted publication message and report
   that the deferred OLE object is not current.
6. The publication can consequently be treated as external, incomplete, or
   replaced before Explorer observes the promised-file formats.

Publication must therefore use a synchronous cross-thread window message and
return the result of `OleSetClipboard()`.

The private ownership marker written by `empty()` is expected to be replaced
when `OleSetClipboard()` installs the OLE data object. Ownership after that
point must be determined using `OleIsCurrentClipboard(m_dataObject)`. Depending
on the old marker after OLE publication would be incorrect unless the marker
were explicitly exposed as another format by the OLE `IDataObject`.

### Deferred-path assessment

The suggested diagnostics are useful if native publication still fails after
the concrete fixes. Registry lookup currently uses an exact UTF-8 path key.
There is no normalization for case, slash direction, relative components, or
Unicode normalization. A path altered anywhere between registration and
platform publication would therefore make `contains()` return false and bypass
the promised-file path.

This diagnostic should remain after the two concrete publication fixes in the
investigation order. It should not be used as a substitute for correcting the
known native publication defects.

## Implemented changes

1. macOS now uses only `prepareForNewContentsWithOptions:` followed by
   `writeObjects:` for deferred file URLs. The legacy `declareTypes:owner:` call
   and redundant `clearContents` call were removed.
2. Windows deferred OLE publication now uses `SendMessageW`, blocks until the
   OLE STA finishes `OleSetClipboard()`, and returns/logs the actual publication
   result.
3. Path diagnostics were intentionally not added before bidirectional runtime
   retesting, preserving the requested investigation order.
