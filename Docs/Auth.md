# Signed admission tokens and pinned DTLS

Every player and inspector connection requires an Ed25519-signed JWT. There is no anonymous or development admission bypass. The dedicated server receives the issuer's **public key only**. The signing private key stays on the offline issuer host. Each token authorizes one role and one connection, identified by a random UUIDv4 `jti`; reconnecting requires a newly minted token.

The signed profile is deliberately narrow: header `alg=EdDSA, typ=JWT`; exactly nine claims `iss`, `aud`, `sub`, `role`, `iat`, `nbf`, `exp`, `jti`, and `dtls_fp`. The issuer is `mrfps-issuer`, the default audience is `mrfps`, and roles are `player` or `inspector`. Numeric dates are integer UTC seconds with at most 3600 seconds of lifetime and 5 seconds of server clock tolerance. `dtls_fp` is the active server's 64-character lowercase SHA256 DTLS-certificate fingerprint. Client connections verify the JWT with the public key before using its transport pin.

## Offline issuer

```powershell
.\Scripts\Initialize-AuthTools.ps1
$python = Join-Path $PWD '.local\auth-tools\Scripts\python.exe'
& $python tools\auth\mrfps_tokens.py keygen --directory .local\auth-issuer
```

Key generation prompts for and confirms a passphrase. It creates an encrypted PKCS8 `issuer-private.pem` and an SPKI `issuer-public.pem`. Noninteractive use can supply `--passphrase-file` pointing to a separately protected file. Passphrases are never command-line values or environment variables. New key/token files are created without overwriting existing files, with POSIX mode 0600 or a protected Windows DACL granting only the current user access before writing any contents. `--dev-unencrypted` is an explicit fixture-only option and is never needed for production keys.

Copy only `issuer-public.pem` to the server configuration and clients. Do not put the issuer private key, passphrase file, or bearer tokens into a Docker build context, image, environment, compose file, source control or logs. The Docker build uses an allowlist and copies neither `.local` nor issuer files.

## Server public-key environment

```powershell
.\Scripts\Start-LinuxContainer.ps1 -Build -PublicKeyFile .local\auth-issuer\issuer-public.pem -Audience mrfps
```

The helper forwards only the public PEM as base64 and the audience, including when Docker runs inside WSL. Direct container operation accepts exactly one nonempty value:

- `MRFPS_AUTH_PUBLIC_KEY_B64`: base64 of the entire SPKI public PEM, preferred.
- `MRFPS_AUTH_PUBLIC_KEY_PEM`: the literal public PEM.

`MRFPS_AUTH_AUDIENCE` defaults to `mrfps`. Missing, ambiguous, malformed or wrong-type public-key settings cause entrypoint failure before the game process starts and cannot pass the health check. The private **issuer signing key** is never passed to the server. The server generates its own ephemeral DTLS transport certificate/key at runtime; that key is a separate transport identity, not the issuer key.

Read the public startup marker `MRFPS_AUTH_TRANSPORT_READY fingerprint=<64 lowercase hex>` from the actual server log. Tokens must use that fingerprint. Restarting the container rotates this ephemeral transport identity, so mint fresh tokens after a restart.

## Issue and deliver a connection token

```powershell
$dtlsFingerprint = '<active server fingerprint from its startup log>'
& $python tools\auth\mrfps_tokens.py issue `
  --private-key .local\auth-issuer\issuer-private.pem `
  --subject player-one --role player --audience mrfps --ttl 300 `
  --dtls-fingerprint $dtlsFingerprint --output .local\access\player-one.jwt

& $python tools\auth\mrfps_tokens.py verify `
  --public-key .local\auth-issuer\issuer-public.pem `
  --token-file .local\access\player-one.jwt --role player --audience mrfps `
  --dtls-fingerprint $dtlsFingerprint
```

Issuance writes the JWT to a restricted file and prints metadata only. Verification prints verified claims, not the bearer token; it does not perform the live server replay check. Treat token files as short-lived credentials. Use a fresh output filename and token for each connection/inspector.

Quest deployment requires both client files:

```powershell
.\Scripts\Deploy-Quest.ps1 -Server 192.168.1.10 `
  -TokenFile .local\access\player-one.jwt `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem
```

This ADB provisioning path requires a **development/debuggable APK with working `run-as`**. The helper discovers the app-private data root, stops the app, and streams the two files over adb stdin to `run-as <package>`. It sets `files/MRFPS` to `0700` and writes `0600` temporary files before atomic rename. On this Quest the canonical paths are `/data/user/0/org.mrfps.arena/files/MRFPS/access.jwt` and `issuer-public.pem`. File contents are absent from process arguments and tool output. The former external `/sdcard/Android/data/...` route failed app-UID readability on this device; it is not the credential-delivery path now. A non-debuggable APK needs a separately implemented provisioning flow; this helper fails when `run-as` is unavailable.

Both files must contain **ASCII bytes with no UTF-8/UTF-16 BOM and no NUL**. The issuer CLI writes this format. Before provisioning, the shared `Resolve-MRFPSCredentialFile` checks the raw bytes and rejects NUL or values above 127, without logging any contents. `Saved/Validation/CredentialEncoding-20261028-033740/report.json` records acceptance of existing ASCII token/public files and rejection of a BOM-prefixed public-key fixture. Format preflight is additional to native signature/claim verification.

The helper passes only file paths through `-MRFPSAuthTokenFile` / `-MRFPSAuthPublicKeyFile`. It launches the local arena with `-MRFPSServer=<host>:<port>` and optional `-MRFPSInspector`, using the product's validated connect path. The authenticated LocalPlayer adds the token only to encrypted login options. Travel URLs carry the public `EncryptionToken=mrfps-dtls-v1` marker.

The current connect path creates the native client certificate, waits six seconds before travel, then revalidates the token before `ClientTravel`. EncryptionAck remains synchronous. Account for that startup interval when choosing token lifetime; the credential must still be valid when travel starts.

For desktop observers, issue distinct tokens with role `inspector` and supply one token file per process:

```powershell
.\Scripts\Start-Inspector.ps1 -Server 192.168.1.10 -Count 2 `
  -PublicKeyFile .local\auth-issuer\issuer-public.pem `
  -TokenFiles .local\access\observer-1.jwt,.local\access\observer-2.jwt
```

## Verification scope

The current video30 production image has fresh deployment/public-key audit evidence in `Saved/Validation/LinuxContainer-video30-production-20261028-144449/report.json`: authenticated healthy startup, runtime1 GiB/zero-swap/2-CPU limits, nonroot/read-only hardening, host-matching ELF, eight fail-closed configuration/health negatives and no recognizable active-issuer private-key match across4229 files plus environment. No host signing key was read. This replaces the prior QA image/pin; its hardcoded prepared native scripts are obsolete. It does not upgrade the historical JWT tests below into new-image client acceptance or measured30fps results. Fresh credentials must bind the new active fingerprint.

`tools/auth/test_tokens.py` exercises the real pinned `cryptography` Ed25519 implementation and strict profile, including wrong signatures/audience/role, expiry, time/type/TTL boundaries, algorithm confusion, unsafe claims, canonical encoding, encrypted-key round-trip and restricted file creation. `Scripts/test_deployment.py` checks fail-closed configuration and actual Linux UDP socket ownership. These are not substitutes for actual UE admission tests or a real game-container run.

`Test-Multiplayer.ps1` uses disposable fixture keys outside the container, waits for the actual server transport fingerprint, then issues a distinct signed token for every client. Only public-key environment data reaches its server process. `Test-AuthAdmission.ps1` separately exercises two valid role connections and seven negative cases for each role: missing/fake/expired/wrong-audience/wrong-role credentials, replay, and a correctly signed token pinned to a different DTLS certificate fingerprint. The pin case must produce the native TLS fingerprint-validation failure, add no server join and produce no server JWT transport-rejection marker. Its result does not claim that the server parsed the JWT. Client-side credential rejection is distinguished from a server rejection; no production bypass is enabled to force invalid credentials through trusted clients.

The existing **protocol2 Linux Docker** was tested with one actual Windows NullRHI client at a time in `LinuxSecurityLifecycle-20261028-104253` and `LinuxSecurityRejections-20261028-104748`. `Saved/Validation/linux-security-protocol2-summary.json` consolidates **22 passed cases**: both valid roles, both server-side replay rejections, both accepted sessions expired/kicked by Linux, ten client-local invalid-credential gates, two native wrong-pin rejections before JWT transmission, and four actual Linux signature/audience rejections. The forged-signature cases deliberately give the test client an attacker's public key while pinning the real server; the wrong-audience cases configure that client's expected audience differently. Native crypto remains enabled and the server still uses its original issuer key/audience. No trusted production client bypass was introduced.

The memory-only UDP observer counted **3795 structurally valid DTLS encrypted-application record headers** over both directions. It checked all eight Unreal bit offsets and ASCII/UTF16 token/full40-character fragments/JWT patterns, with zero matches; no raw packets were persisted or decrypted. Current complete server logs also contained no tested token/full fragment/JWT pattern/private PEM. This is bounded observed traffic, not a guarantee about all future traffic. Missing/wrong-role/expired-at-admission cases rejected locally are **not** Linux server-parser evidence; post-admission expiry was actually verified for both roles.

Both guards exited0 (lifecycle minimum free16.20 GiB/maximum commit46.48%; rejections15.94 GiB/47.08%). Container ID/start/fingerprint stayed unchanged and healthy. The initial `LinuxSecurityLifecycle-20261028-104022` failed before network activity because the newly enabled ProceduralMeshComponent module did not match the frozen Editor graph; it exercised zero auth cases. Successful runs omitted only that unbuilt visualization plugin for the frozen protocol2 DLL. They do not validate later protocol3 geometry sources or a final packaged Windows executable. `Scripts/Test-LinuxSecurity.ps1` runs the repeatable serial probe under the resource guard; encrypted issuer keys use `-PassphraseFile`, and unencrypted disposable fixtures require explicit `-DevUnencrypted`.

`Saved/Validation/LinuxContainer-protocol3-qa-20261028-113432/report.json` records the later protocol3 **QA-only ScoreToWin=1** deployment. Its host-matching ELF, authenticated startup, healthy identity, actual1 GiB RAM/zero-swap/2-CPU limits and all eight configuration/health negatives passed. A public-key-only audit of4228 files plus environment found no recognizable active-issuer private key, and no host signing key was read. Debug/sym stay outside the image. This audit retains its explicit unknown-raw-encoding limitation and is not a protocol3 JWT-admission, multiplayer, geometry or physical-device pass. The prepared native multiplayer command mints fresh, distinct host-issued credentials only when invoked after the Windows package/runtime slot is ready. The separate production restore plan pins the same immutable image, removes the QA process override without rebuilding, and requires fresh tokens for the replacement process's new fingerprint.

The prepared `run-native-security.ps1` in that evidence directory runs all22 serial cases before multiplayer under the10 GiB/75% resource guard. `Test-LinuxSecurity.ps1 -Executable <Windows-package\MRFPS.exe>` explicitly selects packaged mode and resolves an existing inner native game; it omits the Editor project/`-game` arguments. Omit `-Executable` for Editor mode. The old frozen-protocol2 plugin omission is forbidden in packaged mode. `-DryRun` writes a prepared-only plan before signer/Docker initialization, so readiness can be checked with no signing-key read or UE launch. Native protocol3 case results are still pending; the older protocol2 results above are not reused as their pass evidence.

The earlier Quest run `Saved/Validation/Quest-20261028-122912` read private credentials and verified the server certificate, but the server rejected client-certificate `notBefore` with approximately 0.5 s clock skew. That attempt remains a failure. The final destination/pretravel correction has an Editor build at `Editor-guarded-destination-20261028-034804` with exit0. `AuthAdmission-destination-20261028-034846/report.json` records both valid roles/all 14 negative cases passed without credential leak or fatal error. The intermediate `AuthAdmission-dtls-clock-20261028-033754` valid-player timeout is also retained as a failed run.

`Saved/Validation/Quest-20261028-125823/admission-report.json` now records **successful actual Quest cold-process admission** through the Windows UDP relay to the Linux Docker game server. Logs show the six-second wait at 03:58:27Z followed by native certificate verification/handshake, server acceptance of the fresh signed player subject, slot0 join and remote-map loading at 03:58:33Z. All 18 checks passed without a credential leak and the same container remained healthy. Sustained sessions, on-headset visuals, positional tracking, gestures and physical haptics are outside that result. JWT time tolerance and X.509 certificate validity remain different checks.

Primary cryptographic API references: [Ed25519 signing](https://cryptography.io/en/latest/hazmat/primitives/asymmetric/ed25519/) and [key serialization](https://cryptography.io/en/latest/hazmat/primitives/asymmetric/serialization/). The tool environment pins `cryptography==50.0.2`; it implements no signing primitive itself.
