# Quest 3 operation

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

## References

- [Meta: Building and Packaging Apps](https://developers.meta.com/vr/documentation/unreal/unreal-ide-guide-android/)
- [Meta: Unreal Insights on Quest](https://developers.meta.com/vr/documentation/unreal/ts-unreal-insights-ue5/)
- [Meta: Android Debug Bridge](https://developers.meta.com/vr/documentation/unity/ts-adb/)

For the chosen activity launch mechanism, see the installed engine's `Engine/Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp`: it reads the `cmdline` intent string before the packaged command line.
