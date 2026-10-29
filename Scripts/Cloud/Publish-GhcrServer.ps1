[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BundleDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedServerSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [string]$Repository='mizuamedesu/MRFPS-Server-Registry',
    [string]$SourceRepository='mizuamedesu/MRFPS',
    [string]$PrivacyReport,
    [long]$PrivacyWorkflowRunId,
    [long]$RegistryProbeRunId,
    [ValidatePattern('^mrfps-private-[0-9a-f]{16}$')][string]$RunnerLabel,
    [ValidateSet('Plan','Publish')][string]$Mode='Plan'
)
$ErrorActionPreference='Stop'
if($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$'){throw 'REPOSITORY_INVALID'}
if($SourceRepository -ne 'mizuamedesu/MRFPS' -or $Repository -ne 'mizuamedesu/MRFPS-Server-Registry'){throw 'DEDICATED_PRIVATE_PUBLISHER_REQUIRED'}
$BundleDirectory=(Resolve-Path -LiteralPath $BundleDirectory).ProviderPath
$archive=Join-Path $BundleDirectory linux-server.tar.gz
$artifact=Get-Content (Join-Path $BundleDirectory server-artifact.json) -Raw|ConvertFrom-Json
if($artifact.server_sha256 -ne $ExpectedServerSha256){throw 'APPROVED_FRESH_SERVER_HASH_REQUIRED'}
if($artifact.source_commit -ne $SourceCommit){throw 'BUNDLE_SOURCE_COMMIT_MISMATCH'}
$hash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if($hash -ne $artifact.bundle_sha256){throw 'BUNDLE_DIGEST_CHANGED'}
$tag='server-artifacts-'+$hash.Substring(0,12)+'-'+$SourceCommit.Substring(0,12)
$image='ghcr.io/'+$Repository.Split('/')[0].ToLowerInvariant()+'/mrfps-server-private:linux-'+$hash.Substring(0,12)+'-'+$SourceCommit.Substring(0,12)
$plan=[ordered]@{repository=$Repository;sourceRepository=$SourceRepository;sourceCommit=$SourceCommit;bundleSourceCommitMatched=$true;approvedServerSha256=$ExpectedServerSha256;bundleSha256=$hash;releaseTag=$tag;releaseIsDraft=$true;image=$image;publisherPrivateVerified=$false;packageVisibilityChange=$false;githubSecretsChange=$false;compilationFreshnessProvenByHash=$false;mode=$Mode}
if($Mode -eq 'Plan'){$plan|ConvertTo-Json;exit 0}
$publisher=(& gh api "repos/$Repository" | Out-String)|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $publisher.full_name -ne $Repository -or $publisher.private -ne $true){throw 'PRIVATE_PUBLISHER_REQUIRED_BEFORE_ARTIFACT_UPLOAD'}
$plan.publisherPrivateVerified=$true
if(-not $PrivacyReport -or $RegistryProbeRunId -le 0 -or -not $RunnerLabel){throw 'DRAFT_AND_PRIVATE_REGISTRY_PROBE_EVIDENCE_REQUIRED'}
$privacy=Get-Content -LiteralPath $PrivacyReport -Raw|ConvertFrom-Json
if($privacy.repository -ne $Repository -or -not $privacy.authenticatedDraft -or -not $privacy.anonymousDenied){throw 'DRAFT_ANONYMOUS_DENIAL_NOT_PROVEN'}
$probe=(& gh run view $RegistryProbeRunId --repo $Repository --json status,conclusion,workflowName | Out-String)|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $probe.status -ne 'completed' -or $probe.conclusion -ne 'success' -or $probe.workflowName -ne 'Publish approved Linux server image'){throw 'PRIVATE_REGISTRY_FIXTURE_WORKFLOW_NOT_SUCCESSFUL'}
$probeLog=(& gh run view $RegistryProbeRunId --repo $Repository --log|Out-String)
if($LASTEXITCODE -ne 0 -or $probeLog -notmatch 'MRFPS_GHCR_AUTHENTICATED_FIXTURE_PULL=true' -or $probeLog -notmatch 'MRFPS_PRIVATE_DRAFT_FIXTURE_DOWNLOAD=true'){throw 'PRIVATE_REGISTRY_AND_DRAFT_AUTHENTICATED_PULL_NOT_PROVEN'}
$boundaryLines=@($probeLog -split "`n"|Where-Object {$_ -match '\sMRFPS_GHCR_BOUNDARY \{.*\}\s*$' -and $_ -notmatch '\x1b'})
$privateProven=$false
foreach($line in $boundaryLines){
    $proof=($line -replace '^.*MRFPS_GHCR_BOUNDARY ','').Trim()|ConvertFrom-Json
    if($proof.repository -eq $Repository -and $proof.package -eq 'mrfps-server-private' -and $proof.tag -eq 'privacy-fixture-v1' -and $proof.package_private -eq $true -and $proof.package_id_pinned -eq $true -and $proof.source_label_verified -eq $true -and $proof.anonymous_denied -eq $true){$privateProven=$true}
}
if(-not $privateProven){throw 'PRIVATE_REGISTRY_ANONYMOUS_DENIAL_NOT_PROVEN'}
$remoteCommit=(& gh api "repos/$SourceRepository/commits/$SourceCommit" --jq .sha|Out-String).Trim()
if($LASTEXITCODE -ne 0 -or $remoteCommit -ne $SourceCommit){throw 'REVIEWED_SOURCE_COMMIT_NOT_ON_GITHUB'}
function Find-ArtifactDraft {
    param([ValidateRange(1,8)][int]$Attempts=1,[ValidateRange(0,1000)][int]$DelayMilliseconds=750)
    for($attempt=1;$attempt -le $Attempts;$attempt++){
        $raw=& gh api --paginate --slurp "repos/$Repository/releases?per_page=100"
        if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_LIST_FAILED'}
        $pages=$raw|Out-String|ConvertFrom-Json
        $found=@(foreach($page in $pages){foreach($entry in $page){if($entry.tag_name -eq $tag){$entry}}})
        if($found.Count -gt 1){throw 'DRAFT_ARTIFACT_AMBIGUOUS'}
        if($found.Count -eq 1){return $found[0]}
        if($attempt -lt $Attempts){Start-Sleep -Milliseconds $DelayMilliseconds}
    }
    return $null
}
$release=Find-ArtifactDraft
if(-not $release){
    $notes=Join-Path $BundleDirectory release-notes.txt
    [IO.File]::WriteAllText($notes,"Private draft build input. Source $SourceCommit. ELF $ExpectedServerSha256. Do not publish this release.",[Text.UTF8Encoding]::new($false))
    & gh release create $tag --repo $Repository --draft --target main --title "MRFPS server artifact $($hash.Substring(0,12))" --notes-file $notes
    if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_CREATE_FAILED'}
    $release=Find-ArtifactDraft -Attempts 8
}
if(-not $release -or -not $release.draft){throw 'ARTIFACT_RELEASE_MUST_REMAIN_PRIVATE_DRAFT'}
$existing=@($release.assets|Where-Object {$_.name -eq 'linux-server.tar.gz'})
if($existing.Count){
    if($existing.Count -ne 1 -or $existing[0].digest -ne "sha256:$hash"){throw 'EXISTING_ARTIFACT_DIGEST_MISMATCH_OR_UNVERIFIED'}
}else{
    & gh release upload $tag $archive --repo $Repository
    if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_UPLOAD_FAILED'}
}
& gh workflow run publish-linux-server.yml --repo $Repository --ref main -f operation=Publish -f "runner_label=$RunnerLabel" -f "source_commit=$SourceCommit" -f "release_tag=$tag" -f "bundle_sha256=$hash" -f "server_sha256=$ExpectedServerSha256"
if($LASTEXITCODE -ne 0){throw 'GHCR_WORKFLOW_DISPATCH_FAILED'}
$plan.workflowDispatched=$true
$plan|ConvertTo-Json|Set-Content (Join-Path $BundleDirectory publication-dispatch.json) -Encoding UTF8
$plan|ConvertTo-Json
