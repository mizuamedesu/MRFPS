# Quest 3 and end-to-end acceptance

The Quest 3 with serial `2G0YC28F6X05KT` is **ADB authorized**: Android 14, arm64-v8a and recorded Wi-Fi IPv4 `192.168.1.52`. Actual **cold-process signed admission to the Linux Docker game server has passed**, including native DTLS and remote-map loading. Sustained sessions, on-headset MR alignment, physical tracking, gestures and haptics remain unverified. Check the current address and authorization before deployment.

```powershell
.\Scripts\Test-Environment.ps1 -RequireQuest
```

The command records current evidence at `Saved/Validation/environment.json`, including the authorized device's model, Android version and CPU ABI.

## Recorded scope

| Stage | Recorded result and evidence |
| --- | --- |
| APK and install | Latest private-connect package completed with exit0, APK 131,318,586 bytes, v2 signature verified and installed. `Quest-20261028-125823/admission-report.json` records the tested artifact; `quest-installed.json` retains earlier install evidence. |
| Activity/process | `Saved/Validation/Quest-20261028-114343/report.json` records activity launch and `RuntimeProcessAlive=true`. The activity snapshot was paused/asleep; process presence does not establish rendering or network admission. |
| Alpha | `Saved/Validation/quest-alpha-ue.log` records `AndroidPropagateAlpha = 1` in the installed app. |
| GPU/API startup | After the guarded recook/repackage succeeded at 03:02:27Z, `Quest-20261028-120538/logcat.txt` records Alpha1, MobileMultiView1, DisableVulkanSupport0, Vulkan device initialization and `Native OpenXR passthrough is running`. This confirms native API startup; the headset's visible MR scene and alignment are unverified. |
| Private credentials and cold DTLS | The earlier 12:29 run failed server client-certificate notBefore validation. In `Quest-20261028-125823/admission-report.json`, the cold process waited six seconds, then both peers verified certificates/completed native DTLS, the server accepted fresh player subject `quest-cold-d6d1fefeb9` into slot0, and the Quest loaded the remote map. All 18 checks passed. |
| Remaining Quest acceptance | Sustained sessions, visible MR/alignment, head/hand tracking, shared physical calibration, actual glove gestures and tactile output remain unverified. Sleep limited this run's sustained-session evidence. |
| Linux comparison | `LinuxContainer-20261028-022720-limited` records healthy, actual 1 GiB RAM/swap0/CPU2, eight configuration/health negatives and issuer-key absence across 4219 files plus env. The prior single Windows inspector passed 12 checks. The Quest cold-auth run independently proves its relay-to-Linux path. A two-player physical match and long-duration stability remain unverified. |

All paths in that table are under `Saved/Validation`. `Quest-guarded-private-connect-20261028-035218` packaged with one parallel action and exited0 at 03:53:46Z; minimum free RAM10.09 GiB/maximum commit54.71%. The cold-auth evidence is specific to that artifact. Its supportedDevices normalization still needs the separate generated-manifest count record. MoBits host-side transport passed with GET_DEVICES,65 motion frames,22 PONG/STOP and devices0/ARM0/PLAY0. Separately, `MoBitsQuest-20261028-041111/observer.json` passes native dual HELLO/Connected, private302-byte discovery/mode600 and reverse over a3.062-second window; its Quest motion-frame count was not measured. The holder cleaned up its own app/reverse without ARM/PLAY. Physical output remains unverified.

The current inspector head view rerenders the virtual scene from replicated pose. The requested stream of the **actual Quest view including real passthrough** is additional work in progress and remains unverified. A developer-overlay screencap is not an acceptance image of the MRFPS view or MR alignment.

## Install and launch

```powershell
& .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
  -ScriptFile .\Scripts\Build-MRFPS.ps1 -ScriptArguments @('-Target', 'Quest')
if ($LASTEXITCODE -ne 0) { throw "Quest build stopped (exit $LASTEXITCODE)" }
.\Scripts\Deploy-Quest.ps1 -Apk .\Artifacts\Packages\Android_ASTC\MRFPS-arm64.apk -Server 192.168.1.20 `
  -TokenFile .local\access\quest-player.jwt -PublicKeyFile .local\auth-issuer\issuer-public.pem
```

Use the actual packaged APK filename. Without `-Apk`, the helper selects an APK only when exactly one candidate exists. Use `-Serial` when multiple Android devices are connected. The package is `org.mrfps.arena`. The `cmdline` intent extra specifies the arena and `-MRFPSServer=<host>:<port>`; `-Inspector` adds `-MRFPSInspector`, using the same validated product connect path. Immediately before activity start the helper sends standard `KEYCODE_WAKEUP`; it does not disable the proximity sensor. The helper collects logcat, activity, surface, and process evidence under `Saved/Validation/Quest-<timestamp>`. `-SkipInstall` relaunches an existing installation. `-DryRun` prints actions after checking authorization.

Before install, replace, stop, or launch, the Quest launch helpers inspect the activity dump for known screen-sharing and privacy permission activities. The check is repeated after provisioning and immediately before starting the app. If a prompt is present, or the dump cannot be interpreted, they stop without closing it; handle it on the headset before retrying. This is a point-in-time, best-effort check: a future OS activity with an unrecognized name, or a prompt appearing after inspection, cannot be ruled out by ADB.

This provisioning path requires a development/debuggable APK with `run-as`. The helper streams only the public key and fresh role token over adb stdin as the app UID, atomically replacing files in `/data/user/0/org.mrfps.arena/files/MRFPS` (directory `0700`, files `0600`). The shared preflight rejects non-ASCII/BOM/NUL input before delivery. Only paths go into launch arguments; the issuer private key stays on its host. See [Auth.md](Auth.md) for issuance with the running server's signed DTLS pin and exact encoding evidence. Tokens are single-connection credentials; issue a new one for reconnecting and allow for the six-second preconnect interval.

An alive app process is a smoke check. Full acceptance requires the following observations; record the build/version, device identifiers, server log, inspector logs, and relevant video/captures for each run.

| Requirement | Evidence needed |
| --- | --- |
| Quest standalone MR | On-headset passthrough shows the room with correctly aligned virtual hands, weapon, walls and shields; tracking remains stable while moving around the play area. |
| 1 vs 1 online | Two independent clients join one Linux container server and occupy exactly two combatant slots; further observers do not displace them. Both see the same damage, death, score and round state. |
| Approximately 10 m × 10 m | Both participants calibrate the same arena origin/direction; floor boundary and physical positions agree in that space. |
| Wall gesture | Actual left ContactGlove hand rises from low to high; one replicated wall appears at the intended location, with cooldown and expiry. Tracking loss must not spawn a wall. |
| Mini shield | Actual open left palm creates the shield, closing/losing the hand removes it; replicated position and bullet blocking agree on both clients. |
| MoBits shot/hit feedback | Physical MoBits output occurs on the local player's confirmed fire and received-hit events; shot and hit mappings and intensity are recorded. Packet transmission alone is insufficient. |
| N inspectors and actual Quest stream | Verify independent free/3D player cameras, then compare the requested actual Quest stream, including real passthrough and virtual content, with the headset display. The stream is still work in progress; pose-based rerendering alone does not meet that requirement. |
| Linux container | `docker compose ps` reports healthy; server owns UDP 7777 and remains stable for a match and reconnect. |

The full two-person/glove/MoBits run requires those physical devices and a second participant or independent tracked client. Desktop input and automated RPC tests provide useful regression evidence but do not prove physical gesture, MR alignment, or tactile output.

## References

- [Meta: Building and Packaging Apps](https://developers.meta.com/vr/documentation/unreal/unreal-ide-guide-android/)
- [Meta: Unreal Insights on Quest](https://developers.meta.com/vr/documentation/unreal/ts-unreal-insights-ue5/)
- [Meta: Android Debug Bridge](https://developers.meta.com/vr/documentation/unity/ts-adb/)

For the chosen activity launch mechanism, see the installed engine's `Engine/Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp`: it reads the `cmdline` intent string before the packaged command line.
