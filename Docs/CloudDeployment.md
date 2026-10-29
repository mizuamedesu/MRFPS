# Cloud deployment

Server publication is restricted to the dedicated **private** repository
[`mizuamedesu/MRFPS-Server-Registry`](https://github.com/mizuamedesu/MRFPS-Server-Registry)
and the separate package `ghcr.io/mizuamedesu/mrfps-server-private`.
The public repository's publisher is disabled and also refuses public-repository
execution before artifact or registry access. A protocol-14 server image built
from source commit `73754b10926c751cc1e2e362d496db12f89dc44f` is published at
`ghcr.io/mizuamedesu/mrfps-server-private@sha256:3baa4f1e1c0bc4c1313eeab85bc875c840e4d3685667515b56701d23b0c0a888`.
It includes the three-round rules and solo Bot. Later client UI and automation
changes are not part of that image's build provenance.

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

Use an immutable `ghcr.io/...@sha256:...` image and the public key generated by
the production management panel. Only `MRFPS_AUTH_PUBLIC_KEY_B64` is configured;
the private key/passphrase is never read or transferred. CPU is 2000 millicores,
memory 1024 MiB, and one replica is required for the current game state and
ephemeral DTLS identity. Flash's 8 GiB total disk budget includes the image;
the proposed writable rootfs budget is 2 GiB.

Flash currently forces container UID0. The separate Flash image drops to
UID10001, removes every capability and enables no-new-privileges before
executing the existing entrypoint. Its healthcheck also drops to UID10001 so
it can inspect the game's `/proc` descriptors. UE's root refusal stays enabled.
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

It persists a unique deployment marker before creation, rejects a same-name
unowned service, detects specification drift and updates only its saved service.
An ambiguous response stops rather than adopting a resource. No cloud mutation
has been tested: login is still missing. Default `apply` fails before any CLI
mutation because the API cannot allocate public UDP7777. The explicit
`--validation-allocated-port` switch is for a separately authorized cloud QA
test and never counts as satisfying that port requirement; it additionally
requires `--operator-private-pull-ready`. That flag is an operator assertion,
not evidence that a real workload pulled the private image. Runtime game health,
external UDP reachability and the actual fingerprint remain separate checks.

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
pod-scoped WebSocket exec route guarded by `flash:ExecInstance`; actual cloud
authentication and that route remain unverified. A supported log/fingerprint
retrieval path must work before unattended restart recovery is complete. Once
the actual value is obtained, the existing panel CLI can perform the normal GUI
update using `panel-control.mjs fingerprint --profile ... --credentials-file ...
--fingerprint ...`. It does not require reading the issuer private-key backup.

Primary platform specifications:

- [Flash port allocation contract](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud/blob/b6b038a577d133df6dd41ec7c05475232ae6e531/contracts/api/v1/flash.openapi.json#L1495)
- [Flash domains and L4 semantics](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud-Flash/blob/97c2a8962c9af7742964cd78f8e8a254b4af0b6d/docs/AUTOSCALING_AND_DOMAINS.md)
- [Flash registry integration](https://github.com/IPA-CyberLab/IPA-RS-HeteroCloud-Flash/blob/97c2a8962c9af7742964cd78f8e8a254b4af0b6d/src/bin/flash-controller.rs#L34)
- [GHCR authentication](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)
- [Workflow-created package inheritance](https://docs.github.com/en/packages/managing-github-packages-using-github-actions-workflows/publishing-and-installing-a-package-with-github-actions#default-permissions-and-access-settings-for-packages-modified-through-workflows)
