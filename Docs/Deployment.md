# MRFPS build and dedicated server

Use Unreal Engine **5.8**. The build helper checks `Engine/Build/Build.version` rather than silently selecting another engine. This workstation has a source engine at `C:\UnrealEngine\UE5.8` (5.8.1) and cross compiler `C:\UnrealToolchains\v26_clang-20.1.8-rockylinux8`.

Run from the repository root in PowerShell:

```powershell
.\Scripts\Test-Environment.ps1
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

The relay discovers the current WSL address, creates an independent upstream UDP socket per client, and sends responses back to the originating endpoint. It records packet counts at `Saved/Validation/quest-relay.json`. `-TargetAddress`, `-TargetPort`, `-BindAddress` and `-BindPort` override discovery. Windows firewall still needs to allow the chosen inbound UDP port on the relevant LAN interface; the script does not alter firewall rules. `Scripts/Test-QuestRelay.ps1` checks two actual independent connections locally. `-WslContainer` also routes those packets through a real Linux Docker echo fixture and removes its own test container afterwards. This network regression is separate from the game server test.

The readiness check verifies that the **MRFPSServer process owns its game UDP socket**. It rejects unrelated processes listening on that port. It does not establish that a two-player MR match or physical devices have passed acceptance. Verify health with:

```sh
docker compose -f Deploy/Linux/compose.yaml ps
docker compose -f Deploy/Linux/compose.yaml exec mrfps python3 /usr/local/bin/mrfps-healthcheck.py
```

`Scripts/test_deployment.py` runs Linux integration tests against actual `/proc` sockets, including shutdown and unrelated-socket rejection.

## Desktop multiplayer regression

After the editor build/setup, the smoke harness starts its own hidden dedicated editor server, two desktop clients, multiple observers and an extra combatant that should be rejected:

```powershell
.\Scripts\Test-Multiplayer.ps1 -Inspectors 3 -DurationSeconds 90
```

It checks role/slot allocation, match start, calibration, wall generation, damage, shield blocking, and replicated two-player/wall snapshots in every inspector. Logs and a JSON report are written under `Saved/Validation/Multiplayer-<timestamp>`. The default test port is UDP 17777 to avoid a running container on 7777. Teardown stops only process handles created by that invocation. This harness validates desktop networking and game rules, not headset passthrough or physical hardware.

Observers load first; each must receive its inspector role before either fighter starts. Fighter 0 then obtains/calibrates slot 0 before fighter 1 starts. Combat starts only after both are ready. Startup waits check the exact processes created by the harness. The generated arena map and game DLL are required before it will launch any Unreal process. Each inspector must receive damage and walls and log both player views plus return to free view.

## Linux authority multiplayer regression

`Scripts/Test-LinuxMultiplayer.ps1` targets an existing, empty Linux QA server built with the same current network protocol as the clients. Its five primary clients are two synthetic desktop fighters and three independent NullRHI inspectors; a sixth client tests arena-full rejection and exits. Both authenticated fighters join before the CLI requests either calibration, so a temporary one-human Bot match cannot consume the scripted-combat timer. The harness checks the server's elimination and each inspector's matching kill score while the match is playing. Match and combat markers must follow both unique fighter joins, and the inspected score snapshots must be written after the server elimination has been observed. View checks establish replicated POV selection, not rendered pixels or physical Quest gameplay. This p14 workflow has unit coverage but has not yet been verified against a p14 server and package.

Select the QA-only automatic two-player start when deploying the QA image:

```powershell
.\Scripts\Start-LinuxContainer.ps1 -Build -HostPort 17779 `
  -PublicKeyFile <issuer-public.pem> -SmokeValidation
```

`-SmokeValidation` adds `Deploy/Linux/compose.smoke.yaml`: `-MRFPSSmoke` and an override disabling the administrator-start requirement for that smoke test. Each round still lasts 180 seconds; eliminations do not terminate a round early. Authentication stays enabled. Compose retains the same1 GiB RAM/swap-total limit,2 CPUs, dropped capabilities and read-only root filesystem. Recreating this process changes its DTLS fingerprint; obtain the current startup fingerprint before running the test. Keep the outside-guard WSL keepalive alive throughout deployment and testing.

```powershell
# Production signer keys are encrypted; the passphrase file remains host-only.
.\Scripts\Test-LinuxMultiplayer.ps1 -Server 172.23.166.33 -Port 17779 `
  -PrivateKeyFile <host-only-issuer-private.pem> -PassphraseFile <host-only-passphrase-file> `
  -PublicKeyFile <issuer-public.pem> -Fingerprint <current-64-lowercase-hex-pin> `
  -Executable <Windows-package\MRFPS.exe> -StableSeconds 30
```

The packaged executable must be a Development build with the evidence log markers enabled. An outer `MRFPS.exe` package launcher is resolved to its native game executable when present. Omit `-Executable` to use the built UnrealEditor game fallback. The wrapper automatically contains its child tree in the10 GiB free/75% commit resource guard. The clients receive process-scoped `UE_SKIP_UBT_SDK_SETUP=1`, `MSBUILDDISABLENODEREUSE=1` and a two-processor .NET limit. It mints distinct one-connection,3600-second JWT files just before launching each client; only public verification material and tokens reach clients. `-DevUnencrypted` is an explicit opt-in solely for a disposable unencrypted test issuer. `-DryRun` writes a prepared-only plan without reading the signer, contacting Docker or starting UE.

Reports are saved under `Saved/Validation/LinuxMultiplayer-<timestamp>` and its `-guard` sibling. A `STOP` file in that run directory aborts the test; teardown addresses only the processes it created, with remaining descendants contained by the guard's private Windows job. A memory-guard stop is a failed/incomplete attempt, never an automatic retry or stability pass. The default 30-second hold can be changed to 15–600 seconds. Even a longer hold verifies combat and connection stability only: `round_result_verified` and `match_result_verified` remain false. Session-expiry tests are separate, and physical tracking, gestures, haptics and dynamic geometry require their own evidence.

After QA, deploy again **without `-SmokeValidation`** to restore the production administrator-start setting. Verify the new process/fingerprint and issue fresh credentials for it. The old QA tokens cannot be reused against the replacement certificate. Earlier protocol3 runs documented below used the now-removed score override and do not prove the current round rules.

## Consolidated evidence

```powershell
.\Scripts\Get-ValidationSummary.ps1
```

This reads local reports/logs, checks installed Android files, and inspects current project-scoped UnrealBuildTool processes. It does not start Unreal, ADB or Docker. A live build is reported as `in_progress` even if its log has not recently grown. The latest action count and child compiler count are included when available. The result is written to `Saved/Validation/summary.json`; `-NoWrite` only prints it. `-BuildLog <path>` adds a build log, which must reference this exact project to be accepted.

Automated success is kept at its actual scope. The summarizer checks the current source-registered MRFPS automation test names, retains failure/unfinished states, requires the current multiplayer checks and their backing logs, and marks old passes `stale` after source changes. Source timestamp freshness is only a heuristic. Physical MR, arena calibration, ContactGlove, MoBits, rendered inspector views and full Linux multiplayer/device acceptance remain separate gates. The summary never declares the whole user objective complete; that requires inspection of all authoritative evidence and a completion audit.

`Scripts/Test-ValidationSummary.ps1` verifies report parsing against isolated fixtures: unrelated build logs, partial test runs, missing backing logs, failed runs, stale results, missing terminal status and unsupported physical pass claims. Fixture results are not game validation.

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

The detached player windows display the Quest's shared display stream, including passthrough when the headset compositor supplies it and the user has granted sharing. They show a waiting message if no fresh stream is available; they do not substitute the pose-based reconstructed camera. The existing capture implementation does not include audio. Prior single-Quest passthrough observations do not prove simultaneous two-Quest, two-observer operation or sustained frame rate; validate those on the intended network and hardware.

## UE automation

After the editor build and map generation:

```powershell
.\Scripts\Test-Automation.ps1
```

The runner starts **two separate UnrealEditor-Cmd processes**, one for `Automation RunTests MRFPS` and one for `Automation RunTests MoBits.SDK`. It checks native process exit codes, freshly exported reports, source-registered test counts/names, and per-test terminal states. Logs and reports are saved to absolute paths under `Saved/Validation/Automation/MRFPS` and `MoBitsSDK`. `-DryRun` prints the commands without starting either process.

The checkpoint `Saved/Validation/Automation-dtls-clock-20261028-033628` records ten MRFPS and three SDK Success states with process exit0. It predates the latest destination change. The upstream MoBits interop tests report success without opening a connection when their external FakeBackend fixture is absent. The runner lists `RecoveryInterop` and `RustInterop` in `InteropNotExercised`; those two optional paths were not exercised. The final destination admission pass is backed separately by the run above. The earlier post-clock valid-player timeout remains a failed run; neither record establishes Quest admission or physical haptics.

## Evidence on this workstation

LinuxServer BuildCookRun/package succeeded in `Saved/BuildLogs/LinuxServer-Development-20261028-014111.log`. The actual game image was then built and started; `Saved/Validation/LinuxContainer-20261028-022720-limited/report.json` records **healthy**, actual cgroup RAM **1 GiB**, swap **0**, CPU **2**, no restart and no OOM. Eight actual-image negative cases passed: six invalid public-key/audience configurations exited78 before game startup, and two missing-key/missing-game health cases exited1. `issuer-key-audit.json` scanned **4219 files** in the container filesystem/Saved plus env for the actual issuer's private-key bytes, PKCS8, PEM and common encodings; none were found and the configured public key matched. The issuer private key stayed on its host.

`Saved/Validation/LinuxInspectorProbe-20261028-023906/report.json` records **12 passed checks** for one Windows NullRHI inspector connected to that Linux server with a fresh, dedicated token. Actual logs contain native certificate verification, both native DTLS handshake completions, the same subject's inspector admission/join and replicated inspector snapshots. Its client was cleaned up. The container retained its identity and healthy state after cleanup; logged credentials were absent. The guard recorded minimum free RAM 12.49 GiB and maximum commit44.43%. This proves a single authenticated desktop inspector connection; a Linux two-player match, multiple concurrent Linux inspectors, rendered views and physical Quest/MR/haptics acceptance remain pending.

The measured endpoint was `172.23.166.33:17779/UDP` (container port7777). Check the current WSL address and published port before reuse, and issue tokens against the running fingerprint. For this port mapping the LAN relay needs `-TargetPort 17779`; its default upstream is7777. Historical Docker/Compose installation, `hello-world` and UDP echo fixtures are environment evidence, separate from the game-container evidence above.

The final package was rebuilt/deployed in `Saved/Validation/LinuxContainer-final-20261028-041331/report.json`. The successful symbol-excluded attempt exited0, minimum free10.58 GiB/maximum commit55.78%; the earlier guard stop at9.88 GiB is retained as a failed attempt. New image `099c16a1...` started at04:18:58Z, healthy with the same1GiB/swap0/CPU2 caps. All eight negatives passed again. Runtime ELF SHA256 `91222e40cbdeb9d718d9a392a14c259061b89d6267e6c765b4985f909b549f7e` matches the host final package; debug/sym are absent from the image and retained on the host.

Its issuer audit uses only the public key: recognizable PEM/PKCS8/textual seed forms across4219 files plus env produced no active-issuer match; no host signing key was read or token minted. Unknown raw-binary/encoding forms are explicitly outside that audit's assurance. The same report records cgroup values, log privacy and the new fingerprint. Earlier cold-auth evidence applies to its recorded image; client acceptance after this rebuild/rotation needs its own run.

The later **video/protocol2, project0.2** package is deployed in `Saved/Validation/LinuxContainer-video-final-20261028-053516/report.json`. Guarded image build/deploy exited0 (minimum free15.76 GiB, maximum commit45.30%). Image `82253535...` contains the host-matching177,739,320-byte ELF, SHA256 `ebc4ed7ce7583f58532739b5d28ba8e31328af54309e18d40b8cf26e7a85ca6f`; host debug/sym files remain excluded. Its eight isolated configuration/health negatives passed again, actual limits remain1GiB/swap0/CPU2, and the public-only issuer audit found no active-issuer match across4221 filesystem/Saved files plus env. The audit retains its unknown-raw-encoding limitation.

This deployment's initial Quest attempt passed native DTLS and player admission/slot0 join at05:38:59Z, then the host process exited143 at05:39:00Z. Several real startups followed despite `RestartCount=0`; they are preserved as lifecycle failures rather than authentication failures. With the outside-guard keepalive retained, PID667/start05:41:48Z and container start05:41:49Z remained unchanged through05:53:04Z. Two independent probes were130.09 seconds apart, healthy with fingerprint `964b42e0b79587e846c5d4e550fdbce3f85ca99098bd35c0c8cd66de00709b2c`. Last3000-line log privacy checks found no compact JWT/private PEM. The report separates historical runtime samples from current lifecycle evidence; sustained client sessions and visible Quest-stream acceptance still require their own results.

The MoBits app build and host SDK diagnostic passed (`MoBitsBuild-20261028-032811`, `MoBitsSDK-20261028-033615`): control/motion HELLO, GET_DEVICES,65 motion frames,22 PONG and STOP, with zero devices/ARM/PLAY. `MoBitsQuest-20261028-041111/observer.json` separately confirms native Quest dual authenticated HELLO/Connected, internal302-byte/mode600 discovery and reverse over3.062 seconds. Quest motion-frame count was not measured; no SDK commands were sent by the observer. The holder cleaned up only its owned app/reverse/discovery without ARM/PLAY. Physical feedback, sustained motion and game-event output remain unverified.

`Saved/Validation/linux-security-protocol2-summary.json` adds22 passed serial Linux admission/security cases with no container recreation. It distinguishes six actual Linux server rejections (replay/forged signature/wrong audience for both roles), ten client-local rejections, two native wrong-pin failures, and accepted-session expiry/kick for both roles. The two valid-role connections also passed. A bounded memory-only UDP proxy observed3795 DTLS encrypted-application record headers and zero tested token/full-fragment/JWT matches across all8 Unreal bit offsets and ASCII/UTF16; raw packets were neither saved nor decrypted. Complete server-log token/fragment/JWT/private-PEM checks were also clear. The guards exited0 with minimum free16.20/15.94 GiB and maximum commit46.48/47.08%; the same healthy server identity was retained. Direct server parsing of locally rejected missing/role/expired-at-admission tokens remains unproven. These results apply to the frozen protocol2 binaries, with only the newly unbuilt ProceduralMeshComponent visualization plugin omitted from those test clients; they do not validate restored protocol3 geometry or packaged Windows output.

The protocol3 package is now deployed as an explicit **QA ScoreToWin=1** server in `Saved/Validation/LinuxContainer-protocol3-qa-20261028-113432/report.json`. Build/deploy and low-load audits exited0 under the resource guard (minimum free12.23/12.21 GiB). The177,784,440-byte runtime ELF SHA256 `aa483520fc08bccea9531de679dc882f8d1566b5a0b0ff15fdd68e67483594ed` matches the host package; symbols remain host-only. Actual cgroup limits are1 GiB RAM, zero swap and2 CPUs. Eight network-isolated configuration/health negatives passed, and the public-only issuer audit found no active-issuer key match across4228 files plus environment. The new process is healthy without restart/OOM; its fingerprint and exact QA command are recorded in the report. No signing private key was read, no client was launched, and protocol3 gameplay/geometry acceptance is pending. The one-time clean page-cache reclaim preserved the same server ID/start/fingerprint; its before/after measurements are retained separately and do not claim an immediate host-memory gain during the concurrent Windows build.

That run directory contains three prepared commands: `run-native-security.ps1` (22 serial authentication cases), `run-native-multiplayer.ps1` (five primary clients plus arena-full rejection and the300-second hold), and `restore-production.ps1` (same immutable image, no rebuild/pull, map-only production command and new fingerprint). Run them in that order only after the Windows Development package is complete and its runtime slot is assigned. The first two mint fresh fixture credentials at execution time; the restore script reads only the public key and reports resource/public-key/identity checks afterwards. All three use the resource guard. Syntax/mock/planning checks do not establish their actual native game result.

The subsequent **video30 production** deployment supersedes that historical QA instance. `Saved/Validation/LinuxContainer-video30-production-20261028-144449/report.json` records image `7f3471ba...`, container `dd541275...`, start14:48:38Z and a healthy map-only command with no smoke/score override. The177,796,728-byte runtime ELF SHA256 `ba15ddb39cf1a577de04b5ed478140ab5bb689daeedd5bd03416854338c6255e` matches the new host package. Nonroot10001:10001, read-only root, dropped capabilities, no-new-privileges and actual1 GiB/zero-swap/2-CPU limits passed; eight isolated configuration/health negatives and the public-only4229-file-plus-env issuer audit also passed. No signing key was read and symbols remain host-only. The guard exited0 with minimum free14.04 GiB/maximum commit50.27%. This is deployment/audit evidence; actual30fps throughput and new client acceptance are separate measurements. Prior run-specific QA scripts/pins refer to the replaced image and must not be reused. Root owns fresh credentials for this production fingerprint.

## Primary references

- [Epic: Setting Up Dedicated Servers](https://dev.epicgames.com/documentation/unreal-engine/setting-up-dedicated-servers-in-unreal-engine)
- [Epic: Build Operations](https://dev.epicgames.com/documentation/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine)
- [Epic: Linux Development Quickstart](https://dev.epicgames.com/documentation/unreal-engine/linux-development-quickstart-for-unreal-engine)
- [Meta: Building and Packaging Apps](https://developers.meta.com/vr/documentation/unreal/unreal-ide-guide-android/)

The installed engine's source files are authoritative for this build: `Engine/Config/Android/Android_SDK.json`, `Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs`, and `Engine/Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp`.
