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
  against the stream ID of the GPU-acknowledged displayed frame. A zero or
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
The default 640×360 source needs no scale change. Larger/differently shaped
sources use integer nearest sampling, avoiding a large float intermediate.

Each slot holds at most one pending encoded frame. One shared permit spans the
worker, decoded result and RHI upload across all receiver instances. A newer
pending sequence replaces only the pending frame. An earlier worker result may
still advance the display if it belongs to the current stream and local epoch,
is fresh, and is newer than the GPU-acknowledged displayed sequence. This avoids
starvation when decoding finishes after another frame arrives. The latest pending
frame can then advance the display in turn; old generations, resets and display
regressions remain rejected. Dispatch is capped at 5 Hz per slot.
Texture objects are created once and reused; dimension changes do not
create an accumulating texture/GC backlog. The two texture backings, one bounded
source decode and one canvas/upload fit the approximately 20 MiB dedicated-buffer
budget; the UE world, codec module code and graphics driver memory are separate.

Only a ready RHI resource accepts an upload. Buffer and region ownership survive
until the engine's cleanup callback; that callback touches only a thread-safe
mailbox. The game thread publishes visibility after the upload acknowledgment
and another generation/sequence/TTL check. No UObject is accessed from the worker
or cleanup callback. One-second freshness is measured from complete-frame receipt
using local monotonic time, not from decode completion or the remote capture clock.

Dedicated-server and NullRHI instances reject submission, create no texture and
start no worker. The dedicated-server translation unit selects a minimal stub,
excluding the client codec, renderer, worker and video-test implementation at
compile time. Client UObject ticking begins in Initialize and stops before
teardown. Teardown cancels delivery and waits only for its bounded local codec task.

The following automation tests are implemented in MRFPSVideoReceiver.cpp:

- `MRFPS.Video.JpegValidation`: a real engine-generated JPEG, declared dimensions,
  byte cap, truncated/overflowing markers and bounded malformed input.
- `MRFPS.Video.NativeDecode`: real JPEG decode, channel order, opaque letterbox,
  fixed output size and an independent post-decode dimension check. No GPU/network.
- `MRFPS.Video.FrameLifecycle`: ordering, exact TTL expiry, slower-worker progress
  with newer pending input, displayed-sequence regression rejection, exact expected
  displayed-stream matching, changed generations, reset fencing and sequence wrap.

The final UE automation run completed at **2026-10-28T05:25:25Z** with all
**20 results successful**: 17 MRFPS tests (including these three receiver tests
and the bounded relay/protocol tests) and three MoBits SDK tests, with no errors
or warnings. See the actual
[automation report](../Saved/Validation/Video-final-builds-20261028-052419/Automation/report.json).
The report separately marks external MoBits recovery/Rust interop as unexercised
in this run; successful automation results do not establish physical output.

Actual Quest capture, authenticated live relay into a visible Windows inspector,
and visual confirmation that the image contains real passthrough remain integration
checks. The automation results do not establish that path, and a role snapshot,
"No real video" state or reconstructed 3D view is not evidence of live Quest video.
