# Signed admission tokens and pinned DTLS

Every player, inspector and admin connection requires an Ed25519-signed JWT. There is no anonymous or development admission bypass. The dedicated server receives the issuer's **public key only**. The signing private key stays on the offline issuer host. Each token authorizes one role and one connection, identified by a random UUIDv4 `jti`; reconnecting requires a newly minted token.

The signed profile is deliberately narrow: header `alg=EdDSA, typ=JWT`; exactly nine claims `iss`, `aud`, `sub`, `role`, `iat`, `nbf`, `exp`, `jti`, and `dtls_fp`. The issuer is `mrfps-issuer`, the default audience is `mrfps`, and roles are `player`, `inspector`, or `admin`. Numeric dates are integer UTC seconds with at most 3600 seconds of lifetime and 5 seconds of server clock tolerance. `dtls_fp` is the active server's 64-character lowercase SHA256 DTLS-certificate fingerprint. Client connections verify the JWT with the public key before using its transport pin.

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
