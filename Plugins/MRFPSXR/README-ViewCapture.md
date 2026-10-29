The `UMRFPSViewCaptureSubsystem` captures the Android/Quest display composite for
screen sharing. It consumes the OS-provided MediaProjection Surface, including
the content Horizon OS chooses to share. It does not reconstruct passthrough
from camera images or render a replacement viewpoint. Continuous capture on the
current headset firmware still requires an actual consent-and-frame test.

Call `RequestCapture()` on the game thread in response to the local capture
control. It opens the OS consent activity for every new session. A second call
while a request/session is active is idempotent. Denial, OS stop, activity
destruction, or `StopCapture()` ends the session. A stopped/cancelled grant is
never reused. A cancelled consent dialog must return before another request
can be opened.

`IsCapturing()` is true only after a frame arrived in the last two seconds.
`GetSafeStatus()` provides fixed diagnostic text. Poll
`DequeueLatestFrame(Jpeg, Width, Height)` on the game thread; a successful call
moves out the newest pending frame once. The Android worker and JNI bridge use
one bounded mailbox and never queue per-frame game-thread tasks. Stop and
staleness discard pending data. Downstream viewers must also expire their last
displayed frame when the sender stops or stops delivering frames.

The foreground service requests video only, uses a 640x360 RGBA ImageReader with
two image slots, and targets at most 30 frames per second. It reuses its pixel,
bitmap and capped JPEG output buffers. JPEG qualities 65, 50, 35, 20, 12 and 8
are attempted; a frame that still exceeds 40 KiB is dropped. Native code checks the
byte limit, JPEG SOI/EOI and encoded SOF dimensions before accepting a frame.
No capture is saved to disk and no audio, microphone or camera permission is
requested. Server calls return unsupported/false/empty safely.

The separate `MRFPSViewCapture_UPL.xml` adds Android's normal foreground service
and mediaProjection foreground service permissions and an unexported service.
It copies the two Java files and inserts result/destroy hooks into UE's existing
GameActivity. Compile SDK 34 or later is required; the current project uses 35,
target 34 and minimum 32. The service starts foreground before obtaining the
MediaProjection and registers `Callback.onStop` before creating one display.
Shutdown releases the virtual display, ImageReader, projection and worker.
No OS consent is granted or bypassed by this code.

Explicit ADB debug input
----------------------

Only `-MRFPSVideoFromAdb` changes the selected source to the application's
internal `getFilesDir()/MRFPS/Video/frame.bin`. It does not request or start a
MediaProjection session. There is no external-storage fallback. The producer
must write a temporary file in that directory and atomically rename it to
`frame.bin`. The fixed 24-byte header is:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `MRV1` |
| 4 | little-endian uint32 | Width, 1..1280 |
| 8 | little-endian uint32 | Height, 1..720 |
| 12 | little-endian uint32 | JPEG byte count, at most 61440 |
| 16 | little-endian uint64 | Producer UTC Unix nanoseconds, `time.time_ns()` |
| 24 | bytes | Exactly that JPEG, without trailing bytes |

The native receiver rejects zero/repeated/reversed sequences, timestamps more
than two seconds old or more than five seconds ahead, malformed bounds, and
JPEG dimensions inconsistent with the header. A leftover file cannot become
live merely because the game instance restarted. Polling is capped to 5 Hz.
This route is a development aid for an explicit authorized ADB producer; the
product capture path remains user-consented MediaProjection.

Windows player-view sharing
---------------------------

Windows can share its own rendered game viewport through the same JPEG API.
Start it with an explicit local share action (`RequestCapture()`, the player's
X share/stop control), or opt in to automatic sharing after authenticated player
admission with `-MRFPSCaptureDesktopView`. The flag alone does not capture before
the caller requests a session. There is no automatic start without an explicit
action/opt-in, and no fallback from `-MRFPSVideoFromAdb` to Windows game capture.

Only `GetGameInstance()->GetGameViewportClient()->Viewport` is accepted. The
viewport's client and game-instance ownership are checked on every capture.
The public `OnViewportRendered(FViewport*)` delegate is filtered to that exact
viewport; other windows, editor viewports, other game instances, the Windows
desktop and external cameras are never selected. The captured render target
contains the game's Canvas HUD. It does not use a global screenshot request or
capture the containing desktop/Slate window.

Readback, resize and encoding run on the game thread at no more than 2 Hz.
Input must be at most 1920x1080, bounding the raw pixel array to 8,294,400 bytes.
The full image is reduced without upsampling, preserving aspect ratio within
640x360. JPEG qualities 65, 50, 35 and 20 are attempted; output over 60 KiB is
dropped. There is one pending JPEG and no asynchronous frame queue. ReadPixels
does synchronously wait for rendering, so this conservative path can cause a
short game-thread stall and is intended for low-rate sharing.

A session starts empty and must observe a new render of its own viewport.
Frame age starts at that render callback, not after readback/encoding. No draw
means no new capture; two seconds without a fresh frame makes IsCapturing false
and dequeue empty. Failed readback/encoding immediately clears pending data.
Stop/deinitialization removes the render delegate and discards its frame; a new
session cannot reuse an old frame. NullRHI, dedicated servers, non-rendering
processes and missing game viewports are unsupported.

For a real relay check, start a rendered Windows player at 1280x720 using its
normal server address and signed-token/public-key files, add
`-MRFPSCaptureDesktopView`, then join a separate rendered authenticated inspector
to the same server and select that player's POV. Change the player's in-game
camera or HUD, verify the inspector follows it, then press X in the player to
stop and verify the inspector loses the signal. The core owns authenticated
publication and subscription policy; this subsystem changes no RPC or wire
format. This checks the Windows game-view source and existing relay separately.
It does **not** verify Quest passthrough/HUD capture. Native Quest continuous
MediaProjection capture and real headset content remain unverified until a
consented on-device capture-and-frame test succeeds.

Primary sources and validation
------------------------------

- [Meta MediaProjection API](https://developers.meta.com/vr/documentation/native/native-media-projection/):
  Horizon OS directly populates the supplied Surface; its scaling and
  VirtualDisplay resize behavior can differ from base Android. This API is for
  user-consented casting, streaming and screen sharing.
- [Android MediaProjection](https://developer.android.com/media/grow/media-projection):
  target-34 foreground service permissions, one-use consent, callback and
  resource cleanup requirements, and full-display selection.
- Local UE 5.8 primary source:
  `Engine/Source/Runtime/Core/Public/Android/AndroidJavaEnv.h` supplies the
  exported JNI environment/activity/class lookup APIs. The GameActivity
  template and `UEDeployAndroid.cs` provide the activity result/destroy UPL
  insertion points. `UnrealPluginLanguage.cs` defines PluginDir and resource
  copy processing. No engine source was modified.
- Windows UE 5.8 primary source: `UnrealClient.cpp:1868` flushes the scene
  canvas and broadcasts `UGameViewportClient::OnViewportRendered` before
  screenshot processing and EndRenderFrame. `FRenderTarget::ReadPixels`
  (`UnrealClient.cpp:61`) enqueues ReadSurfaceData and flushes rendering.
  `SceneViewport.cpp:2285` installs the direct-render RT target, and its
  EndRenderFrame releases that reference; capture therefore occurs inside the
  rendered-viewport callback instead of a later Tick. `ImageCore.h` provides
  bounded `ResizeImageAllocDest`; `ImageUtils.h:133` provides CompressImage.
  The only added Windows module dependency is ImageCore; JPEG encoding uses
  Engine's existing ImageWrapper-backed public helper.

`Saved/Validation/ViewCapture-static/api-check.json` records a read-only check
against the installed Android 34 `android.jar`: 19 required method signatures,
both Java source-copy paths, JNI callback names, and both UE GameActivity hooks.
This check parses class-file metadata; it neither compiles nor starts capture.
The two Java sources also compile independently with JDK 17.0.14 and Android
34's `android.jar`, using `-J-Xmx128m -source 17 -target 17 -Xlint:all -Werror`:
exit 0, no warnings/errors, no stubs. Outputs and logs are ignored under
`.local/viewcapture-javac/`; compiled JNI descriptors are recorded there too.
This does not run Gradle, package an APK, or start a device capture.
UE/UHT compilation, APK packaging, foreground/consent lifecycle, headset
composite content and live delivery remain runtime validation steps.
