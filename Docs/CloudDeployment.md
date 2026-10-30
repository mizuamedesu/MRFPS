# Cloud deployment

Server publication is restricted to the dedicated **private** repository
[`mizuamedesu/MRFPS-Server-Registry`](https://github.com/mizuamedesu/MRFPS-Server-Registry)
and the separate package `ghcr.io/mizuamedesu/mrfps-server-private`.
The public repository's publisher is disabled and also refuses public-repository
execution before artifact or registry access. The latest published protocol-14
server image was built from source commit `bc0a8d683a2b7ece5c0ff933be6de1e90afef1fc`:
`ghcr.io/mizuamedesu/mrfps-server-private@sha256:9a04ded4dabdba0e45b16176d1c91b54900c18e2df053aa71e93fefcd027b4ab`.
It includes the three-round rules and solo Bot. Later changes are not part of
that image's build provenance. Publishing it is not a Flash deployment.

GitHub workflow-created packages inherit the publishing repository's visibility.
The first public-repository attempt consequently exposed the server image; the
single exact version/package was removed in
[retraction run 40922179525](https://github.com/mizuamedesu/MRFPS/actions/runs/40922179525).
Fresh anonymous tag/digest manifests and layer requests then failed, including
the previously issued anonymous layer credential. This cannot revoke copies
that might already have been downloaded. No issuer private key was included.
The old public package name is never reused for publication.

## Source and artifact provenance

1. Freeze and commit the complete server source, configuration, Docker launchers
   and publication tools. Push that reviewed commit through the maintainer.
2. Build/package LinuxServer from that unchanged source under the existing
   10 GiB free-memory / 75% commit guard, one native compile action at a time.
   Keep the real build/UAT logs and source revision with the build record.
3. Confirm that no relevant source changed during the build. Compute the actual
   ELF SHA256 and create a new bundle with the same full source commit:

   ```powershell
   .local/auth-tools/Scripts/python.exe tools/cloud/server_bundle.py bundle `
     --package <approved-new-LinuxServer-directory> `
     --server-sha256 <approved-ELF-sha256> --source-commit <full-reviewed-commit> `
     --output Saved/Deployments/GHCR/<build-name>
   ```

   Neither an ELF hash nor a file timestamp alone proves that a binary was
   built from that commit. Rebuild/repackage when the source changes; do not
   relabel an old bundle with a newer source revision. The publisher rejects a
   disagreement between its source argument and `server-artifact.json`.
4. Verify the non-secret draft fixture's anonymous denial in the private
   publisher, then run its harmless image/draft Probe with an ephemeral runner.
   Only after that succeeds may an approved new bundle be published:

   ```powershell
   Scripts/Cloud/Publish-GhcrServer.ps1 -BundleDirectory <bundle-directory> `
     -ExpectedServerSha256 <approved-ELF-sha256> -SourceCommit <same-full-commit>
   Scripts/Cloud/Invoke-PrivatePublisherRunner.ps1 -Operation Probe `
     -SourceCommit <same-full-commit> -PrivacyTag <private-nonsecret-fixture-tag>
   Scripts/Cloud/Invoke-PrivatePublisherRunner.ps1 -Operation Publish `
     -BundleDirectory <bundle-directory> -ExpectedServerSha256 <approved-ELF-sha256> `
     -SourceCommit <same-full-commit> -PrivacyReport <private-draft-report.json> `
     -RegistryProbeRunId <successful-private-Probe-run-id>
   ```

The archive is a deterministic, bounded whitelist of the packaged server.
Saved files, `.debug`, `.sym` and credential files are excluded; symbols remain
on the build host. Extraction rejects traversal, links, duplicate paths, and
unexpected files before writing. The workflow checks both archive and ELF
digests, takes deployment code from the reviewed commit, audits missing-key
startup under UIDs 0 and 10001, and records the pushed OCI digest.

Release inputs remain private drafts in the private publisher. Draft lookup uses
the authenticated release list and a bounded retry after creation; GitHub's
by-tag REST endpoint does not return unpublished drafts. Publication uses that
private repository's scoped `GITHUB_TOKEN`, with `packages: write` and
`contents: write` for draft downloads. Repository/package metadata, authenticated
pull, anonymous tag/digest/largest-layer denial, and private draft download are
verified using an innocuous fixture before a game binary is sent. OCI source
linkage points at the private publisher; a separate label records the public
source repository and its reviewed commit. Visibility is never broadened.

The private package is pinned to immutable ID `16392074`. The harmless
[Probe run 40953878450](https://github.com/mizuamedesu/MRFPS-Server-Registry/actions/runs/40953878450)
verified that identity, a scoped authenticated pull and anonymous denial. The
[Publish run 40954210256](https://github.com/mizuamedesu/MRFPS-Server-Registry/actions/runs/40954210256)
pulled the real published image and verified the server ELF SHA-256
`4598e7965dcae2b970b1ad2d4ab6c3963bf23ad5c826e8a9aaa287869c070314`;
anonymous tag, digest and largest-layer requests were denied. The runner and
its owned temporary artifacts were removed afterward. These workflow checks
used its scoped job token; the local host's credential could not inspect package
metadata independently. A conflicting repository link or changed ID fails.
`-Operation Inspect` performs only the scoped metadata/version inspection and
does not start a BuildKit builder.

The later [Publish run 40956828932](https://github.com/mizuamedesu/MRFPS-Server-Registry/actions/runs/40956828932)
published the `bc0a8d6` image above and verified the actual pulled ELF SHA-256
`52e48e2c598f2dca8d2a3e6686b86e4ac35ddb8f790adf9293c7ff36527a8db9`.
Its scoped job token performed the authenticated pull; anonymous manifest and
layer requests were denied. The separate local Quest test container was not
replaced by this publish and is not evidence of this digest running in Flash.

GitHub-hosted private Actions were blocked by the account's payment/spending
setting. The scripts do not change billing. A task-specific ephemeral Linux
runner uses an exact random label, one job, and no service installation. The
registration token travels only over stdin. Its transient cgroup is limited
to 1 GiB and no swap; a separate owned BuildKit container is limited to 1.5 GiB,
no additional swap, CPU2 and one build action. The host keeps the existing
10 GiB-free / 75% commit guard. An independent WSL watchdog treats a stale guard
heartbeat as failure and stops only recorded groups/that task's cgroup and
builder. Cleanup removes the job's credentials and runner registration, without
pruning shared Docker data or stopping existing game containers. ICU is extracted
from a verified Ubuntu package into the runner directory and used through that
process's `LD_LIBRARY_PATH`; no system installation or global environment changes
are made.

The Linux runner verifies the pinned official runner archive before extracting
it. On the verified glibc host it omits only the unused Alpine Node 20/24
runtimes; host Node 20/24 remain available. Credentials and runner state stay
in `/tmp` after confirming it is tmpfs. The audited server archive and build
context use a task-specific, mode-0700 directory on verified ext4 under
`/var/tmp`, so those large files can be reclaimed under the runner's 1 GiB
cgroup limit. This directory is never used for tokens, private keys, or Docker
credentials. Its marker, owner, permissions, and directory identity are
verified before task-scoped cleanup; no shared directories are pruned.

After an abrupt WSL shutdown, RAM-only state and the watchdog disappear while
the disk directory may survive. Once the exact interrupted report is available,
recover only that task with:

```powershell
Scripts/Cloud/Recover-PrivatePublisherRunner.ps1 `
  -ReportPath Saved/Validation/PrivateServerPublish-<run>/report.json
```

The command is idempotent. It checks the report's repository/task/runner label,
the directory's marker and ownership, and that the task's tmpfs state,
systemd unit, original manager PID, and uniquely named builder are absent.
A remaining builder, unreadable Docker state, conflicting identity, missing
marker, or any uncertainty prevents deletion and is reported for inspection.
If interruption occurs before the ownership marker is durable, an empty
directory may remain and automatic recovery deliberately refuses it.

## Flash compatibility and current deployment gates

On 2026-10-30 the owned `mrfps-server` Flash service
`01a18a87-c459-7563-bc3b-397306404251` ran generation 4 in `direct` mode at
`163.220.236.61:30000/UDP`. Its published image is
`ghcr.io/mizuamedesu/mrfps-server-public-e30271e1965be539@sha256:96d6930bc598aa2ea2004733ca698508bbbaa35c53d124f71afb44a642190708`
(source `2aae110ba45e7379ed6ce44565301488ad41a32b`, server ELF SHA-256
`26f5ab98d69fb967c7b783569975b1137a0cff03ae32cadbf97e557e02d2a4eb`).
The real Pod's healthcheck passed, and a Windows inspector and Quest player
both completed DTLS, authentication and JOIN. Quest video frames also reached
the Windows client's fresh decoded texture; this is not a visual screenshot or
a long-term stability test. Address, port and fingerprint must be rechecked
after future service changes.

At that time `mrfps.mizuame.app` did not resolve, and the available Wrangler
OAuth token could not read or edit DNS records (API 403). A DNS-only A record
for `mrfps` pointing to the actual Flash IP is needed; even after DNS resolves,
clients must use the assigned port, currently `30000`, rather than `7777`.

Use an immutable `ghcr.io/...@sha256:...` image and the public key generated by
the production management panel. Only `MRFPS_AUTH_PUBLIC_KEY_B64` is configured;
the private key/passphrase is never read or transferred. CPU is 2000 millicores,
memory 1024 MiB, and one replica is required for the current game state and
ephemeral DTLS identity. Flash's 8 GiB total disk budget includes the image;
the proposed writable rootfs budget is 2 GiB.

The public IP endpoint uses Flash `traffic_mode: direct`. The provider applies
`externalTrafficPolicy: Local`; its `load_balancer` and `web` endpoint modes
require forwarded traffic and are not used here. Direct mode still uses the
tenant API's assigned public service port and does not itself allocate UDP7777.

Flash currently forces container UID0. The separate Flash image drops to
UID10001, removes every capability and enables no-new-privileges before
executing the existing entrypoint. Its healthcheck also drops to UID10001;
where gVisor blocks `/proc` descriptor inspection it uses the authenticated
local health responder described in [FlashHealth.md](FlashHealth.md).
UE's root refusal stays enabled.
Flash itself does not expose the same read-only filesystem setting as Docker
Compose, so local `--read-only` success is not evidence of that cloud setting.

Two platform requirements remain explicit:

- The tenant API replaces requested `service_port` with a port in **30000–32767**.
  A DNS A/CNAME does not translate ports. Fixed public UDP7777 requires a
  platform-supported dedicated allocation or an independently approved L4
  relay. Changing an application's default port does not satisfy that requirement.
- Private GHCR pulls require the operator's registry integration, including
  both authenticated image inspection and the workload imagePullSecret.
  Registry credentials are not tenant application environment variables.

The Flash controller supports `FLASH_REGISTRY_HOST`,
`FLASH_REGISTRY_USERNAME`, `FLASH_REGISTRY_PASSWORD_FILE` and
`FLASH_REGISTRY_PULL_SECRET` together. Availability of those settings on the
actual cluster must be confirmed; defaults in source are not live-state proof.

`tools/cloud/flash_service.py` provides offline `plan`, owned-service `status`,
and guarded `apply` reconciliation through the installed HeteroCloud CLI:

```powershell
.local/auth-tools/Scripts/python.exe tools/cloud/flash_service.py plan `
  --image <approved-private-ghcr-image@sha256:digest> `
  --public-key C:/Users/ilike/AppData/Local/MRFPS/Admin/issuer-public.pem `
  --project-id <actual-project-UUID> --state Saved/Deployments/Flash/service.json
.local/auth-tools/Scripts/python.exe tools/cloud/flash_service.py status `
  --project-id <same-project-UUID> --state Saved/Deployments/Flash/service.json
```

For an already-owned public, direct-mode service, a new immutable server image
and the roster publisher key can be reconciled without writing the Flash
manifest or key to disk:

```powershell
.local/auth-tools/Scripts/python.exe tools/cloud/update_owned_flash.py `
  --state Saved/Deployments/Flash/service.json `
  --image ghcr.io/<owner>/<server>@sha256:<digest> `
  --roster-key-file "$env:LOCALAPPDATA/MRFPS/Admin/flash-roster/roster-hmac.key"
```

The command preserves the existing UDP service port and direct inbound mode,
checks ownership and specification drift, and allows outbound traffic only to
the current Cloudflare IP ranges after checking that the admin hostname resolves
inside them. DNS is separately allowed by Flash. The allowlist covers shared
Cloudflare IPs rather than a single hostname; the publisher also verifies the
HTTPS peer and signs each roster. An ambiguous update is recorded as a digest
and must be observed before another update is attempted. A server restart
rotates its DTLS fingerprint and requires clients to receive a new token.

It persists a unique deployment marker before creation, rejects a same-name
unowned service, detects specification drift and updates only its saved service.
An ambiguous response stops rather than adopting a resource. Creation and
updates of the owned Flash service have been exercised. Default `apply` fails before any CLI
mutation because the API cannot allocate public UDP7777. The explicit
`--validation-allocated-port` switch is for a separately authorized cloud QA
test and never counts as satisfying that port requirement. It additionally
requires either the `--operator-private-pull-ready` assertion or an explicit
`--attempt-private-pull` to test a real deployment while marking operator and
runtime private-pull readiness unverified. An assertion is not evidence that a
real workload pulled the private image. Runtime game health,
external UDP reachability and the actual fingerprint remain separate checks.

The installed CLI v0.1.110's JSON service list is a bare array: it unwraps the
API's `items` envelope. The [corresponding API contract](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud/blob/4e4d2a67e995de70aa9898ae195cde8d3174cb0e/contracts/api/v1/flash.openapi.json)
describes the list as having no pagination limit. The CLI discards any extra
envelope fields, so an array alone cannot prove completeness if a deployed
server later changes that contract. Before applying, verify that the live API
uses the same unpaginated list contract. Unknown CLI output shapes fail closed.

Cloudflare's ordinary orange-cloud HTTP proxy cannot forward the game's UDP.
Use DNS-only records for an already reachable game endpoint. General UDP port
translation through Spectrum requires the appropriate existing entitlement;
this workflow creates no subscription. `heterocloud dns reconcile` installs or
updates a cluster ExternalDNS controller and multiple platform records, so it
is not a tenant-scoped substitute for missing DNS Edit permission.

## Runtime fingerprint

Every server restart rotates the native DTLS certificate. Obtain the actual
`MRFPS_AUTH_TRANSPORT_READY fingerprint=<64 lowercase hex>` marker from the
new instance, verify stable service identity/readiness, update the management
panel, and only then issue fresh single-use client tokens. Never reuse an old
fingerprint or infer it from an image digest.

The current HeteroCloud CLI has no `flash logs` command. The management API has a
pod-scoped WebSocket exec route guarded by `flash:ExecInstance`; this route and
fingerprint retrieval were verified against the running service. Automatic
fingerprint refresh after an unattended restart is not yet implemented. Once
the actual value is obtained, the existing panel CLI can perform the normal GUI
update using `panel-control.mjs fingerprint --profile ... --credentials-file ...
--fingerprint ...`. It does not require reading the issuer private-key backup.

For the owned service, `node Deploy/CloudflareAdmin/flash-runtime.mjs --state
<service.json> --expected-image <exact-image@sha256:digest>` verifies the saved
ownership marker, immutable image, observed generation, unchanged specification
and single ready Pod before using the authenticated API's container shell. It
reads only the healthcheck result, process identity and fingerprint startup
marker; the CLI login token stays in memory. It repeats the probe and checks the
Pod, process and generation again before reporting the fingerprint. The command
requires the admin dependencies (`npm ci` in `Deploy/CloudflareAdmin`). Socket
health is not proof of public UDP reachability or successful client admission.
Those were independently demonstrated for the 2026-10-30 generation above.

Primary platform specifications:

- [Flash port allocation contract](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud/blob/b6b038a577d133df6dd41ec7c05475232ae6e531/contracts/api/v1/flash.openapi.json#L1495)
- [Flash domains and L4 semantics](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud-Flash/blob/97c2a8962c9af7742964cd78f8e8a254b4af0b6d/docs/AUTOSCALING_AND_DOMAINS.md)
- [Flash registry integration](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud-Flash/blob/97c2a8962c9af7742964cd78f8e8a254b4af0b6d/src/bin/flash-controller.rs#L34)
- [GHCR authentication](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)
- [Workflow-created package inheritance](https://docs.github.com/en/packages/managing-github-packages-using-github-actions-workflows/publishing-and-installing-a-package-with-github-actions#default-permissions-and-access-settings-for-packages-modified-through-workflows)
