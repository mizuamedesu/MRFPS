# MRFPS build and dedicated server

Use Unreal Engine **5.8**. The build helper checks `Engine/Build/Build.version` rather than silently selecting another engine. This workstation has a source engine at `C:\UnrealEngine\UE5.8` (5.8.1) and cross compiler `C:\UnrealToolchains\v26_clang-20.1.8-rockylinux8`.

Run from the repository root in PowerShell:

```powershell
foreach ($target in @('Editor', 'LinuxServer', 'Windows', 'Quest')) {
    & .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
        -ScriptFile .\Scripts\Build-MRFPS.ps1 -ScriptArguments @('-Target', $target)
    if ($LASTEXITCODE -ne 0) { throw "Build stopped: $target (exit $LASTEXITCODE)" }
}
```

The resource guard is recommended for builds on this workstation. Before launch and every two seconds it requires at least **10 GiB available physical RAM** and at most **75% of the system commit limit** in use. A failed preflight starts no build. If pressure is detected during execution, it stops only its own private Windows job, including descendants; existing applications are outside that job. Memory rejection or termination returns exit **125**, guard errors return **126**, and normal completion preserves the script's exit code. JSON reports, memory samples and child stdout/stderr go to `Saved/Validation/ResourceGuard-*`; `-ReportPath` selects a fresh explicit report filename.

Run one target when resuming an individual stage:

```powershell
& .\Scripts\Invoke-MRFPSResourceLimited.ps1 `
    -ScriptFile .\Scripts\Build-MRFPS.ps1 -ScriptArguments @('-Target', 'Quest', '-SkipSetup')
if ($LASTEXITCODE -ne 0) { throw "Quest build stopped (exit $LASTEXITCODE)" }
```

Use `-SkipSetup` only when the editor and generated arena are current. `Build-MRFPS.ps1` limits UBT to **two parallel actions** by default (one or two can be selected with `-MaxParallelActions`, also forwarded through UAT). `Config/DefaultEngine.ini` configures **two shader workers on this 16-thread workstation**. The Android UPL sets Gradle's Java heap to **2 GiB**, metaspace to 512 MiB, maximum workers to two, and parallel execution off. These settings reduce concurrent work; the memory guard still checks the whole system. Complete each heavy stage before starting another: editor/setup, platform build/cook/package, Docker image build, then multiplayer/device tests. Do not run those stages concurrently.

`-EngineRoot` overrides discovery. `-DryRun` prints commands without invoking Unreal. Editor builds run `MRFPSSetup` to generate `/Game/Maps/MRFPSArena`. Packaging first ensures the editor module and generated arena exist. `-SkipSetup` is available when both are already known to be current. Build logs go to `Saved/BuildLogs`; archived packages go to `Artifacts/Packages`.

The server target is `MRFPSServer`. Linux server packaging requires a source engine or installed build with Server support and the Linux compiler toolchain. A normal Launcher editor may not provide those targets. The Docker image contains the packaged server and runtime libraries; it does not distribute Unreal Engine source or a development image.

For LinuxServer, the helper builds `ShaderCompileWorker`, `UnrealPak` and `MRFPSServer` in separate `Build.bat` invocations, then packages with UAT `-skipbuild`. Each UBT process releases its dependency graph before the next one starts. The final separated run `Saved/Validation/LinuxServer-guarded-separated-20261028-040525/report.json` completed at04:06:48Z with exit0, minimum free RAM10.60 GiB and maximum commit58.51%. The earlier combined graph reached the guard threshold and is not counted as a pass.

The Android setup reads exact package versions from this engine's `Engine/Config/Android/Android_SDK.json`:

```powershell
.\Scripts\Initialize-Android.ps1
```

This workstation's required packages were installed into the ignored project directory `.local/android`: Android API 34, build-tools 35.0.1, NDK 27.2.12479018 (r27c), and CMake 3.22.1. Existing JDK 17 is used at `C:\Program Files (x86)\Android\openjdk\jdk-17.0.14`. `Build-MRFPS.ps1 -Target Quest` sets SDK/JDK variables only for its process and restores them afterwards.

Quest native build/cook/package, APK v2 signature verification, install, activity launch and PID-alive checks have succeeded. After the profile/multiview recook completed at `2026-10-28T03:02:27Z`, `Saved/Validation/Quest-20261028-120538/logcat.txt` records Alpha1, MobileMultiView1, DisableVulkanSupport0, Vulkan device initialization and native OpenXR passthrough running. These are API startup observations; visible MR and tracking remain unverified.

Deploying credentials requires a development/debuggable APK with `run-as`. `Deploy-Quest.ps1` uses adb stdin and the app UID to atomically provision `/data/user/0/org.mrfps.arena/files/MRFPS` with directory `0700` and files `0600`, then passes file paths and `-MRFPSServer` through the product connect path. The shared helper rejects raw non-ASCII/BOM/NUL input; the issuer CLI writes the required ASCII format. See [Auth](Auth.md) for the encoding test and provisioning contract.

The earlier `Quest-20261028-122912` notBefore rejection is retained as a failed attempt. The final correction creates the client certificate before a six-second pretravel interval, revalidates the token and then travels; EncryptionAck remains synchronous. `Editor-guarded-destination-20261028-034804` completed with exit0 and the destination admission regression passed both roles/all 14 negative cases. `Quest-guarded-private-connect-20261028-035218` then packaged the updated APK with one parallel action and exit0 (minimum free 10.09 GiB/maximum commit 54.71%).

**Actual Quest cold-process admission passed** in `Saved/Validation/Quest-20261028-125823/admission-report.json` (18 checks). The 131,318,586-byte APK was v2-verified and installed. Client/server native certificate verification and handshake, signed player admission/slot0 join and client remote-map load followed the six-second wait at 03:58:27–03:58:33Z. The relay path was `192.168.1.10:7777 -> 172.23.166.33:17779`; the same container remained healthy without restart/OOM or logged credentials. Sleep limited the observation, so sustained sessions, visible MR/alignment, physical tracking, gestures and haptics remain unverified. The supportedDevices metadata normalization has a separate generated-manifest count still to record. [DeviceValidation](DeviceValidation.md) keeps these scopes distinct.

## Container

After LinuxServer packaging:

```powershell
.\Scripts\Start-LinuxContainer.ps1 -Build -PublicKeyFile .local\auth-issuer\issuer-public.pem
.\Scripts\Start-LinuxContainer.ps1 -Status
.\Scripts\Start-LinuxContainer.ps1 -Stop
```

If the Windows Docker CLI is unavailable, this helper invokes Docker inside WSL Ubuntu. Docker Engine and Compose are installed and functional in that distribution. The commands use its root context to access the Docker socket; the **game process itself runs as uid 10001 inside the container**.

For that WSL path, the helper creates a hidden, recorded `wsl ... sleep infinity` process. Its lifetime must also survive the launcher's process/job cleanup. During the final deployment, repeated WSL/container startups rotated the DTLS identity while Docker `RestartCount` remained zero; the older recorded helper was no longer live. A retained `wsl -d Ubuntu --exec /bin/sleep infinity` session started **outside the resource guard** then kept the same actual server PID, start time and fingerprint across independent probes. Keep such a session running for WSL-hosted client validation and verify those three values before issuing tokens. This is bounded stability evidence, not an indefinite availability guarantee. `-Stop` stops only its recorded helper after validating PID, creation time and command line; a separately retained session has its own explicit lifetime. `-HostPort` selects another published UDP port if 7777 is occupied; the game continues to use 7777 inside the container.

Equivalent on a Linux host, from the repository root:

```sh
docker compose -f Deploy/Linux/compose.yaml up -d --build --wait
docker compose -f Deploy/Linux/compose.yaml logs -f
docker compose -f Deploy/Linux/compose.yaml down
```

Before direct Compose startup, set exactly one public-key environment value (`MRFPS_AUTH_PUBLIC_KEY_B64` preferred, or `MRFPS_AUTH_PUBLIC_KEY_PEM`); never supply the issuer private key. Missing or invalid public-key configuration fails before the game starts. See [Auth.md](Auth.md) for encrypted offline key generation and short-lived role tokens pinned to the actual server DTLS fingerprint.

The image copies `Artifacts/Packages/LinuxServer`, exposes **7777/UDP**, starts `/Game/Maps/MRFPSArena`, persists `MRFPS/Saved`, and drops Linux capabilities. Set `MRFPS_HOST_PORT` for another published host port. Inspectors and players connect to that host and port with separate signed connection tokens. For remote devices, route the UDP port through the host's actual LAN/public interface and firewall; WSL's default NAT does not itself guarantee inbound LAN UDP forwarding. The included relay also provides a Windows LAN to WSL UDP path.

`MRFPSServer.debug` and `MRFPSServer.sym` are excluded from the Docker build context and runtime image. The low-memory default Linux build strips DWARF at link time and does not generate Breakpad symbols; those host files do not provide full source-level symbols in that mode. Use `-LinuxPortableSymbols` on a machine with sufficient memory to request full debug information and portable symbols. Other packaged runtime files are retained.

Compose explicitly limits the game service to **1 GiB RAM** (`mem_limit: 1g`), **1 GiB total RAM plus swap** (`memswap_limit: 1g`, disabling container swap), and **two CPUs** (`cpus: 2.0`). These runtime limits apply when Compose creates or recreates the service; editing the file does not change an already running container. They do not limit image builds or the entire WSL VM. The bounded game container still needs a real match/stability check under those limits.

On this PC's default WSL NAT network, an included user-space relay provides the Windows-to-container UDP path without changing WSL configuration:

```powershell
# Keep running in its own PowerShell while clients use the Windows LAN IP.
.\Scripts\Start-QuestRelay.ps1
# Connect Quest/inspectors to this PC's LAN IPv4 address, UDP 7777.
```

The relay discovers the current WSL address, creates an independent upstream UDP socket per client, and sends responses back to the originating endpoint. It records packet counts at `Saved/Validation/quest-relay.json`. `-TargetAddress`, `-TargetPort`, `-BindAddress` and `-BindPort` override discovery. Windows firewall still needs to allow the chosen inbound UDP port on the relevant LAN interface; the script does not alter firewall rules.

The readiness check verifies that the **MRFPSServer process owns its game UDP socket**. It rejects unrelated processes listening on that port. It does not establish that a two-player MR match or physical devices have passed acceptance. Verify health with:

```sh
docker compose -f Deploy/Linux/compose.yaml ps
docker compose -f Deploy/Linux/compose.yaml exec mrfps python3 /usr/local/bin/mrfps-healthcheck.py
```


## Inspectors

Open independent inspector windows against the same dedicated server:

```powershell
.\Scripts\Start-Inspector.ps1 -Server 127.0.0.1 -Count 1 -UseEditor `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem -TokenDirectory .local\access\inspectors
# Or use the packaged Windows client:
.\Scripts\Start-Inspector.ps1 -Server 192.168.1.20 -Count 1 `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem -TokenDirectory .local\access\inspectors
```

The URL includes `?Inspector=1` so observer connections do not claim the two player slots. Press Esc to release the mouse, then click **P1 別窓**, **P2 別窓**, and **観戦 別窓**. Together with the original free camera, these create four separately capturable windows from one authenticated inspector process. The second observer window has its own camera: click it, move with WASD/E/Q, and hold right mouse to look. Closing a player window stops its extra subscription when no other view needs that source. All three detached windows reuse the owning inspector's connection and do not create additional Quest uploads. Additional independent inspector processes or PCs are also supported; each needs its own fresh signed token. Keep the windows open rather than minimized. The desktop client disables the foreground idle throttle so a visible window continues updating when another window has focus.

The detached player windows display the Quest's shared display stream, including passthrough when the headset compositor supplies it and the user has granted sharing. They show a waiting message if no fresh stream is available; they do not substitute the pose-based reconstructed camera. The existing capture implementation does not include audio.

## Primary references

- [Epic: Setting Up Dedicated Servers](https://dev.epicgames.com/documentation/unreal-engine/setting-up-dedicated-servers-in-unreal-engine)
- [Epic: Build Operations](https://dev.epicgames.com/documentation/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine)
- [Epic: Linux Development Quickstart](https://dev.epicgames.com/documentation/unreal-engine/linux-development-quickstart-for-unreal-engine)
- [Meta: Building and Packaging Apps](https://developers.meta.com/vr/documentation/unreal/unreal-ide-guide-android/)

The installed engine's source files are authoritative for this build: `Engine/Config/Android/Android_SDK.json`, `Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs`, and `Engine/Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp`.
