# UE video receiver

`UMRFPSVideoReceiver` is a game-instance subsystem for complete JPEG frames
delivered by the authenticated native game relay. It opens no socket and does
not read, reserve or log an access token. Authorization, publisher/slot ownership,
fragment reassembly, source capture age and per-connection bandwidth belong to
the relay. This component does not establish that a producer captured passthrough.

Call its API on the game thread:

- `SubmitFrame(Slot, StreamId, Sequence, Width, Height, MoveTemp(Jpeg))` returns
  true when header/order validation accepted the frame into the latest queue.
  Actual codec failure is asynchronous; acceptance does not mean a visible image.
- `GetFreshTexture(Slot)` returns an uploaded fixed 640×360 texture, or null.
  The UI must show a no-video/stale state for null, with 3D reconstruction labeled
  separately. It must not silently substitute a SceneCapture under a real-video label.
- `GetFreshTextureForStream(Slot, ExpectedStreamId)` adds an exact comparison
  against the stream ID of the RHI-cleanup-acknowledged frame. A zero or
  different expected ID returns null immediately, even if the old slot image
  remains within its freshness TTL. The inspector uses the authenticated current
  player's server-assigned ID so a replacement player cannot inherit that image.
- `GetSafeStatus(Slot)` contains fixed text and receive age only.
- `ResetSlot(Slot)` blanks the slot and closes its current generation. A higher
  stream ID is required to reopen it. View selection alone should not call this.
- `ResetAll()` clears server-generation watermarks for a new server session and
  increments local epochs so old workers cannot publish into that session.

Slots are 0 and 1. Stream IDs are nonzero, server-assigned, increasing uint32
generations. Sequence numbers are nonzero and strictly increase within a stream;
wrap is rejected. A changed stream immediately invalidates the previous image,
pending frame and every old worker/upload completion.

The receiver accepts at most 60 KiB per JPEG. A bounded marker scan checks SOI,
EOI, segment lengths and an 8-bit baseline/progressive SOF before codec entry.
Encoded dimensions must exactly equal the declared dimensions and be no larger
than 1280×720; only grayscale/RGB JPEG is accepted. The real engine decoder then
rechecks the resulting dimensions, slice count, format and byte count.

ImageCore/ImageWrapper load on the game thread. Worker decode produces opaque
BGRA8 and fits the source onto a fixed 640×360 canvas with black letterboxing.
The default 640×360 BGRA source takes a bounded copy path and forces opaque
alpha. Larger/differently shaped sources use integer nearest sampling, avoiding
a large float intermediate.

Each slot holds at most one pending encoded frame. At most two process-wide
permits span worker decode, decoded results and RHI upload, with at most one
permit per receiver slot. Both player views can therefore proceed independently
in the same tick; an RHI stall still bounds work and buffers. A newer pending
sequence replaces only the pending frame. An earlier worker result may
still advance the display if it belongs to the current stream and local epoch,
is fresh, and is newer than the previously acknowledged sequence. This avoids
starvation when decoding finishes after another frame arrives. The latest pending
frame can then advance the display in turn; old generations, resets and display
regressions remain rejected. Dispatch is capped at 60 Hz per slot and actual
throughput depends on capture, network, codec, game tick and renderer. Texture
objects are created once and reused; dimension changes do not create an
accumulating texture/GC backlog. Two 1280×720 BGRA decode images use up to
7,372,800 bytes and two 640×360 BGRA canvases use 1,843,200 bytes. Codec scratch,
RHI allocations, GPU drivers, pending encoded frames and the UE world are extra.

If multiple GameInstances exist in one process, a process-local FIFO gives a
waiting receiver a turn before a receiver that just ran can reacquire. The
queue stores at most 64 receiver IDs; when full, other receivers retry on later
ticks, and fairness beyond this bound is not guaranteed. This is not a cap on
inspector client processes or windows. Waiting entries expire after 0.5 seconds
without a scheduling attempt and are removed on reset/normal teardown. This
prevents an inactive instance from blocking other receivers indefinitely, but
does not guarantee fairness for a GameInstance that ticks less than twice per
second or stops ticking while it waits.

Only a ready RHI resource accepts an upload. Buffer and region ownership survive
until the engine's cleanup callback; that callback touches only a thread-safe
mailbox. The game thread publishes visibility after the cleanup callback and
another generation/sequence/TTL check. The callback is not a GPU presentation
fence and does not by itself prove a visible image. No UObject is accessed from the worker
or cleanup callback. One-second freshness is measured from complete-frame receipt
using local monotonic time, not from decode completion or the remote capture clock.

Dedicated-server and NullRHI instances reject submission, create no texture and
start no worker. The dedicated-server translation unit selects a minimal stub,
excluding the client codec, renderer, worker and video-test implementation at
compile time. Client UObject ticking begins in Initialize and stops before
teardown. Teardown cancels delivery and waits only for its bounded local codec
tasks; callbacks retain their own mailbox, buffers and permits. UE 5.8's normal
process shutdown flushes the render and RHI queues before module shutdown and
does not free module DLL code until process exit. An explicit Editor dynamic
module unload can instead free code while callbacks remain queued; this module
does not currently drain those callbacks before such an unload. Dynamic module
unload with a running Editor is therefore unverified; close the Editor normally
before rebuilding or reloading this module. Normal process shutdown does not
require a GPU presentation fence.

The following automation tests are implemented in MRFPSVideoReceiver.cpp:

- `MRFPS.Video.JpegValidation`: a real engine-generated JPEG, declared dimensions,
  byte cap, truncated/overflowing markers and bounded malformed input.
- `MRFPS.Video.NativeDecode`: real JPEG decode, channel order, opaque letterbox,
  fixed output size, an independent post-decode dimension check and the direct
  640×360 copy path. No GPU/network.
- `MRFPS.Video.FairScheduling`, `MRFPS.Video.ConcurrentBudget` and
  `MRFPS.Video.ReceiverFairness`: per-view eligibility, process cap, separate
  mailboxes, permit lifetime, receiver ordering and stale-waiter expiry.
- `MRFPS.Video.FrameLifecycle`: ordering, exact TTL expiry, slower-worker progress
  with newer pending input, displayed-sequence regression rejection, exact expected
  displayed-stream matching, changed generations, reset fencing and sequence wrap.

The earlier UE automation run completed at **2026-10-28T05:25:25Z** with all
**20 results successful**: 17 MRFPS tests (including these three receiver tests
and the bounded relay/protocol tests) and three MoBits SDK tests, with no errors
or warnings. See the actual
[automation report](../Saved/Validation/Video-final-builds-20261028-052419/Automation/report.json).
It predates the two-permit change and does not verify it. The independent
`tools/video/test_video_budget.cpp` stress test can also be compiled as a small
C++17 program; it checks the actual atomic budget across several concurrent
threads and multiple receivers without building UE. The report separately marks external MoBits recovery/Rust interop as unexercised
in this run; successful automation results do not establish physical output.

Actual Quest capture, authenticated live relay into a visible Windows inspector,
and visual confirmation that the image contains real passthrough remain integration
checks. The automation results do not establish that path, and a role snapshot,
"No real video" state or reconstructed 3D view is not evidence of live Quest video.
