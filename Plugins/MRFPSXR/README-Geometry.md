`UMRFPSGeometryCaptureSubsystem` publishes a sampled visible surface from the
runtime's real `XR_META_environment_depth` images on Android Vulkan headsets.
Windows and dedicated servers return unsupported/empty. There is no static
Scene snapshot, camera replacement, simulation or fixture input path.

Call RequestCapture on the game thread only for the intended local authenticated
player. The subsystem requests Android's normal Spatial Data permission
`com.oculus.permission.USE_SCENE`; it never grants permission or dismisses OS UI.
The native passthrough feature must be running. StopCapture and deinitialization
invalidate queued results. A stopped request cannot be resurrected by old GPU
work. The core owns authorization, calibration and publication policy.

`DequeueLatestFrame(FMRFPSEnvironmentGeometryFrame&)` moves the one newest frame
once. A false return clears the entire output. The plain frame contains
tracking-space UE centimeters (+X forward, +Y right, +Z up), FVector3f vertices,
uint32 triangle indices, Sequence, TrackingSpaceEpoch and bTrackingValid.
CaptureMonotonicTime is local FPlatformTime at successful acquisition. It is
**not sensor exposure time**; UE's bundled extension v1 has no capture timestamp.
Acquisition/readback age is rejected at 0.75 seconds, so this bounds the local
pipeline's age, not undocumented camera/runtime processing latency.

GetTrackingSpaceEpoch returns a nonzero epoch for a supported live depth session,
including while capture is stopped. It returns zero on unsupported platforms or
without a session. Session change, tracking loss, user-absence events, changed UE
tracking space, base-offset mismatch and recenter events invalidate the epoch
and pending geometry immediately. A pending runtime reference-space change also
invalidates at its effective time. The core must discard geometry and recalibrate
on any changed/lost epoch, and reject frames acquired before its calibration.

The frame coordinates match UE's actual tracking space, including its recenter
and base offsets. During UpdateDeviceLocations, UE retains the supplied XrSpace.
Within that callback only, the plugin locates it relative to an owned identity
space of the same native type (Local, LocalFloor or Stage), then creates an owned
reference space using the **returned offset pose**. Clone-to-actual-space identity
is checked immediately and on every later callback; a mismatch clears geometry
and increments the epoch. The supplied engine handle is retained only as an
opaque comparison ID and is never used later or destroyed by the plugin. Custom
tracking origins fail closed. The core transforms these tracking centimeters
through its calibrated Pawn XROrigin once; it must not apply Engine TrackingToWorld
again. bTrackingValid requires both position/orientation VALID and TRACKED flags
from a real VIEW-space location, a running session and an aligned owned space.

The default capture rate is 2 Hz; mrfps.Geometry.CaptureHz is clamped to 0.5–4 Hz.
One left-eye 48x36 point grid yields at most 1,728 vertices and 3,290 triangles,
below the public 2,048-vertex / 12,288-index limits. Invalid samples stay holes.
Adjacent triangles crossing a depth step greater than 0.15+0.02*depth meters,
edges over 75 cm and degenerate triangles are rejected. Hand-removal infill is
disabled when supported. Samples are limited to 0.25–7 m forward depth, with
finite tracking coordinates within 200 m; far observations are less reliable.

The raw Vulkan path samples the runtime-owned two-layer D16 image in its
SHADER_READ_ONLY_OPTIMAL layout. It never imports it as an RHI render target,
clears it, copies it using unpromised transfer usage, or changes its layout/queue
ownership. Compute writes only a 27,648-byte owned host-visible SSBO. There is
one command buffer/fence in flight and one newest CPU frame, without per-frame
tasks or queues. Fence polling is nonblocking; noncoherent memory is invalidated
after completion. GPU coordinates must agree with the returned view/frustum
reference math within 0.5 cm, or the frame is rejected, never repaired.

Acquisition and submission occur inside the UE5.8 Vulkan OpenXR bridge's existing
serialized graphics-queue callback after successful xrBeginFrame, before
xrEndFrame. This exact route matters: a generic RHI-thread callback does not
establish Vulkan queue synchronization. No nested RHIRunOnQueue or queue wait is
used. Session teardown waits at most two seconds for the owned fence. Normal
completion/device loss releases owned resources before depth swapchain/provider
destruction. An unproven completion quarantines that one bounded native resource
set and disables further depth use for the process; in-flight objects are not
freed. Pathological GPU/runtime device teardown can still fail independently.

This is partial, view-dependent, runtime-estimated depth. Occluded objects,
transparent/reflective surfaces, thin obstacles and fast motion can be missing
or inaccurate. It is not a complete room map or a physical safety system. Actual
moving-obstacle delivery, coordinate alignment, latency and headset permissions
still need a legitimate on-device test. The unavailable/unauthorized Quest has
not been replaced by test fixtures or marked physically verified.

Validation and primary sources
------------------------------

The dependency-free GeometryDepthMath.h is used by runtime metadata, projection
and topology checks. A standalone MSVC C++20 /W4 /WX check runs its same 40 cases:
finite/infinite depth, pixel centers and asymmetric FOV, axes and returned pose,
base-offset mismatch, depth holes/steps, 75 cm cutoff, capacities, NaN and TTL.
This tiny check uses no UE libraries or device. MRFPS.XR.Geometry.DepthAndTopology
and FreshFrameAndEpoch Automation tests additionally cover GPU projection
agreement and the actual mesh/single-consumer output path, including stale,
future, lost tracking and recentered frames. Those UE tests await the root build.

Shaders/BuildEnvironmentShader.ps1 invokes the local NDK glslc for Vulkan1.0 and
optionally spirv-val, generating EnvironmentGeometrySpirv.h. Two independent
builds and validations produced identical header SHA256
`908215804B0B4C5DD901CDF4ADD06F6E6771BB0C6F8977770709DDC67A53B18D`.
Shader source SHA256 is
`9CC0E31D7A4CE0746A9008DEFFCAF997174ED98F178C4C9400D50002301334E9`.
No UE build, app start, capture or device operation was performed for these checks.

- [Khronos environment-depth specification source](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/meta/meta_environment_depth.adoc)
  defines D16 array layers, returned view/frustum, far<near infinite projection,
  acquire/end-frame lifetime and the positive NOT_AVAILABLE result with unchanged
  outputs. This implementation explicitly handles that result without publishing.
- [Meta native Depth overview](https://developers.meta.com/vr/documentation/native/android/mobile-depth/)
  and [Spatial Data Permission](https://developers.meta.com/vr/documentation/native/native-spatial-data-perm/)
  specify the permission declaration/request and the metric depth conventions.
  The documents were also fetched using the official installed Meta VR CLI.
- Local primary UE5.8 sources: OpenXRHMDModule.cpp:994/1342 establish extension
  instance/system order; OpenXRHMD.cpp:1840 retains the current tracking space
  during UpdateDeviceLocations, :1039 maps actual origin types, :4073 applies
  UE's base pose, :3890 and :4453 establish submission/begin-frame callbacks, and
  :2466 flushes rendering before session destruction. OpenXRHMD_RenderBridge.cpp:348
  uses public RHIRunOnQueue(Graphics); VulkanSubmission.cpp:859/885 and
  VulkanQueue.cpp:321/491 show the serialized callback and queue submissions.
  IVulkanDynamicRHI.h provides the public Vulkan device/queue/proc lookups.
