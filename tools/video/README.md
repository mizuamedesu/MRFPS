# Quest ADB video fallback

This development-only producer captures the actual Android display through an
explicit authorized ADB serial. It never launches an app, dismisses overlays or
changes proximity settings. Native MediaProjection is the main capture path;
this is a fallback whose visible content still needs comparison with the headset.

Initialize the isolated environment, then run a bounded producer when the game
and file consumer are ready:

```powershell
.\Scripts\Initialize-VideoTools.ps1
.\Scripts\Start-QuestVideoBridge.ps1 -Serial 2G0YC28F6X05KT -DurationSeconds 15
```

To feed an authenticated Windows development player proxy, explicitly select
an absolute host file instead of the Android-private destination:

```powershell
.\Scripts\Start-QuestVideoBridge.ps1 -Serial 2G0YC28F6X05KT `
  -HostFrameFile (Join-Path $PWD 'Saved\Runtime\QuestVideo\frame.bin') `
  -DurationSeconds 30 -Fps 1 -CaptureTimeout 1.5 -MaxFrameAge 2
```

Python exposes the same option as `--host-frame-file ABSOLUTE_PATH`. This mode
uses only ADB authorization status and read-only screencap: it does not run
`run-as`, create Android directories, or push/clean up any Android frame. The
host file contains the same24-byte MRV1 header and bounded JPEG described below.
A temporary file in the same host directory is flushed/fsynced, then atomically
replaces the selected destination. A stale or failed write preserves the prior
frame and removes only its own temporary file. The consumer must reject the
retained final frame once its original capture-start timestamp is older than2s.
No installed/debuggable Quest APK is required by this capture-to-host mode.
It proves an ADB-tethered input path only when actually run and observed; it does
not establish native Quest gameplay, physical tracking or stream acceptance.

The PowerShell launcher uses the existing resource guard: 10 GiB free physical
RAM, commit at most 75%, checked every 2 seconds, and only its owned process job
can be stopped. `-Watch` is explicit continuous mode; Ctrl+C stops it. The bridge
does not mint tokens or connect a game client. Game admission and authenticated
RPC forwarding remain the game's responsibility.

Default output is 640x360, at most 5fps, JPEG quality75 with quality reductions to
stay within 60 KiB. Scaling preserves the complete aspect ratio with black
letterboxing; `-Eye both` is the default with no crop. `-Eye left/right` explicitly
selects one half of side-by-side input; no stereo or overlay removal is inferred.
`-CaptureFormat raw` supports exact Android 12/16-byte headers and RGB/RGBA/RGBX
8888. PNG is the default; both separately authorized one-frame device benchmarks
returned exit0 with **zero output bytes**. Those benchmark runs established
neither raw support nor valid live PNG capture/speed. Metadata is in
`Saved/Validation/QuestVideoRawBenchmark-20261028-045418/report.json` and
`QuestVideoPngBenchmark-20261028-050824/report.json`. Continuous publication and
the end-to-end stream need separate actual-run evidence. The optional host
destination was added later and has unit/mock coverage only at this checkpoint.

The private device file is `files/MRFPS/Video/frame.bin`: `MRV1` plus little-endian
uint32 width/height/JPEG-length and uint64 capture-start Unix UTC nanoseconds,
then JPEG bytes. The header is24 bytes. Directory mode is700 and the atomic
temporary-file/rename path uses600. A development/debuggable APK with `run-as`
is required for this default Android-private destination. Source dimensions/pixels,
capture output, decoder memory and JPEG length are bounded. ADB stderr and raw
captures are never logged or saved. The optional host destination intentionally
persists one encoded MRV1/JPEG file, and reports that fact in its safe counters.

The timestamp is taken **before** screencap, never after encoding. Default total
frame-age budget is1.5 seconds (maximum2); capture/publish timeouts share it. Late
frames and elapsed schedule slots are dropped/rebased rather than queued or
retimestamped. A clock rollback drops samples rather than fabricating a newer
capture time. The last encoded device frame can remain after exit and must be
rejected by the native timestamp-age rule. Only the producer's own incomplete
temporary file is removed. An OS lock prevents duplicate producers for the same
explicit serial/package on this host.

The only Python dependency is pinned [Pillow13.0.0](https://pypi.org/project/pillow/13.0.0/).
Codec options follow [Pillow's JPEG documentation](https://pillow.readthedocs.io/en/stable/handbook/image-file-formats.html#jpeg).
