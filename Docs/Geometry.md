# Measured environment geometry

The new source path accepts **current observed Environment Depth surfaces** from
an authenticated, calibrated XR player. It does not manufacture obstacles from
the video image, substitute static Scene geometry, or claim to reconstruct a
complete room. The native acquisition and its physical-device accuracy still
require validation; source integration and tests alone are not proof of live data.

The native/core contract is `GeometryCaptureSubsystem.h`:
`UMRFPSGeometryCaptureSubsystem::DequeueLatestFrame` supplies
`FMRFPSEnvironmentGeometryFrame` with tracking-space UE axes (+X forward,
+Y right, +Z up), centimetres, vertices/triangle indices, sequence, tracking-valid
state and tracking-space epoch. `CaptureMonotonicTime` is the local time of a
successful OpenXR acquire, **not camera exposure time**. Native default sampling
is 48×36 at 2 Hz, with a 4 Hz ceiling and one latest pending readback/frame.

Core transforms samples using the real pawn's calibrated `XROrigin`. It waits
for the server to acknowledge a new calibration generation and rejects frames
acquired before that calibration. A changed native tracking-space epoch requires
recalibration. Tracking loss, invalid calibration, disconnect and auth expiry stop
publication and remove the associated server/client geometry.

The native epoch is captured when the player calibrates and checked every local
tick, including when no fresh depth frame is ready. Overlapping calibration/loss
requests each advance the local acknowledgment fence; an earlier server reply
cannot authorize a later transform. Reliable server requests advance that epoch
even during a finished match. Match-phase rules still govern firing independently.

Each snapshot is limited to 2,048 vertices and 4,096 triangles. Coordinates are
quantized to signed 1 cm values. Referenced vertices must lie within arena
X/Y ±500 cm and Z −20..300 cm. Degenerate triangles and edges over 150 cm are
rejected; triangles crossing missing or out-of-arena samples are dropped locally,
without inventing a surface along the clipping boundary. Server samples must also
be within 650 cm of the current tracked head.

`FMRFPSGeometryChunk` has a custom network serializer which checks every count,
derived total and payload length **before allocation**. The encoded maximum is
36 KiB. Each data RPC carries at most 512 bytes; one padded XOR row per eight
data rows can repair one missing row. Each serialized fragment is below 600 bytes
before ordinary RPC/DTLS overhead, within the existing 1,024-byte transport policy.
An assembler owns one incomplete snapshot; sender queues own one current plus
one latest snapshot per slot. Conflicting duplicates, older generations,
completed-sequence replay and partials older than 750 ms are rejected.

Only the player's owned controller may upload. Admission checks the native
encrypted session, authenticated player role/slot, UTC session expiry, actual
pawn ownership, calibration generation, fresh sane tracked head pose, arena
membership, geometry bounds and per-controller byte/update budgets. It does not
require a match to have started, so a lone calibrated player can scan while
waiting. Inspector credentials cannot publish obstacles. Authentication identifies
the publisher; it does not attest the physical sensor or prevent an authorized
modified client from lying about its bounded samples.

Capture age is mapped from the client's monotonic acquire time into the replicated
GameState server-world time estimate. The server accepts at most 750 ms age with
250 ms allowed estimate lead, and atomically replaces the complete snapshot after
full validation. Partial/malformed updates leave the previous complete snapshot
untouched until it expires. A surface expires at 1.5 seconds from mapped acquire
time, not from final-fragment receipt. Recalibration, tracking/ownership loss or
expiry also invalidates collision immediately when checked, without waiting for
the next cleanup tick. A stop closes the old stream; a later valid snapshot gets
a new server-assigned stream ID.

A receiver-only timeout hides the mesh while retaining its owner and sequence
watermarks. A newer complete snapshot in the same still-authorized server stream
can recover after packet loss. Authority expiry or an ownership/calibration change
retires the generation and requires a higher server stream ID. An expired
inspector session clears its locally displayed geometry as well.

The dedicated server stores two bounded triangle sets and uses a two-sided CPU
segment/triangle closest-hit query. It creates no geometry render actors, physics
meshes or per-triangle Chaos bodies. The authoritative bullet compares this hit
with its existing weapon-channel hit: a nearer measured surface blocks damage
and terminates the replicated tracer there; a target/shield/game wall in front
of the surface retains its existing behavior.

Authenticated recipients receive the same bounded fragments over the existing
native DTLS game connection. A current publisher's replicated stream/calibration
IDs must match before committing. Windows inspectors show at most two transient
`UProceduralMeshComponent` meshes, collision disabled, with an existing unlit
material. Server/NullRHI paths create no visualization. Owner changes and stale
data remove the actual mesh instead of displaying a synthetic replacement.
The HUD distinguishes `Live measured surfaces` from `No live measured geometry`.

Source tests added under `MRFPS.Geometry` cover finite/two-sided/closest surface
hits, capture-time bounds, quantization and malformed indices, bounded wire reads
before allocation, loss recovery/duplicate/timeout handling, and actual authority
bullets blocked by a measured surface then restored after removal. Calibration
ordering tests cover overlapping requests, delayed acknowledgments and overflow.
The real-world-subsystem lifetime test uses paused game time and actual monotonic
expiry, comparing authority retirement with client recovery and auth/calibration
revocation. Its client-mode engine driver is never opened or connected.

The game protocol is now **3**, with the procedural-mesh dependency excluded from
dedicated-server targets. These tests have **not been run at this source
checkpoint**; the coordinated Editor/Linux/Quest builds and live integration checks
remain required. Earlier Windows video evidence remains a separate result. No live
depth acquisition or physical obstacle accuracy is claimed from these changes.

Ordinary player rendering also removes the duplicate camera-front status text and
hides one's own world label, retaining HUD status and labels seen externally.
Desktop player pitch now compensates for the configured MouseY scale before
changing its direct FRotator pitch; inspector controller pitch is unchanged.
The next UI check should verify natural up/down motion, own-label absence,
other-player label visibility, and intact PLAYER POV/3D/Esc controls. These are game
rendering/input choices, not edits to or substitutions for captured imagery.
