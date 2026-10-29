[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BundleDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedServerSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [string]$Repository='mizuamedesu/MRFPS',
    [string]$PrivacyReport,
    [long]$PrivacyWorkflowRunId,
    [ValidateSet('Plan','Publish')][string]$Mode='Plan'
)
$ErrorActionPreference='Stop'
if($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$'){throw 'REPOSITORY_INVALID'}
$BundleDirectory=(Resolve-Path -LiteralPath $BundleDirectory).ProviderPath
$archive=Join-Path $BundleDirectory linux-server.tar.gz
$artifact=Get-Content (Join-Path $BundleDirectory server-artifact.json) -Raw|ConvertFrom-Json
if($artifact.server_sha256 -ne $ExpectedServerSha256){throw 'APPROVED_FRESH_SERVER_HASH_REQUIRED'}
if($artifact.source_commit -ne $SourceCommit){throw 'BUNDLE_SOURCE_COMMIT_MISMATCH'}
$hash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if($hash -ne $artifact.bundle_sha256){throw 'BUNDLE_DIGEST_CHANGED'}
$tag='server-artifacts-'+$hash.Substring(0,12)+'-'+$SourceCommit.Substring(0,12)
$image='ghcr.io/'+$Repository.Split('/')[0].ToLowerInvariant()+'/mrfps-server:linux-'+$hash.Substring(0,12)+'-'+$SourceCommit.Substring(0,12)
$plan=[ordered]@{repository=$Repository;sourceCommit=$SourceCommit;bundleSourceCommitMatched=$true;approvedServerSha256=$ExpectedServerSha256;bundleSha256=$hash;releaseTag=$tag;releaseIsDraft=$true;image=$image;packageVisibilityChange=$false;githubSecretsChange=$false;compilationFreshnessProvenByHash=$false;mode=$Mode}
if($Mode -eq 'Plan'){$plan|ConvertTo-Json;exit 0}
if(-not $PrivacyReport -or $PrivacyWorkflowRunId -le 0){throw 'DRAFT_PRIVACY_AND_ACTIONS_PERMISSION_EVIDENCE_REQUIRED'}
$privacy=Get-Content -LiteralPath $PrivacyReport -Raw|ConvertFrom-Json
if($privacy.repository -ne $Repository -or -not $privacy.authenticatedDraft -or -not $privacy.anonymousDenied){throw 'DRAFT_ANONYMOUS_DENIAL_NOT_PROVEN'}
$run=& gh run view $PrivacyWorkflowRunId --repo $Repository --json status,conclusion,workflowName | Out-String|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $run.status -ne 'completed' -or $run.conclusion -ne 'success' -or $run.workflowName -ne 'Check draft artifact access boundaries'){throw 'ACTION_DRAFT_PERMISSION_PROBE_NOT_SUCCESSFUL'}
$remoteCommit=(& gh api "repos/$Repository/commits/$SourceCommit" --jq .sha|Out-String).Trim()
if($LASTEXITCODE -ne 0 -or $remoteCommit -ne $SourceCommit){throw 'REVIEWED_SOURCE_COMMIT_NOT_ON_GITHUB'}
function Find-ArtifactDraft {
    $raw=& gh api --paginate --slurp "repos/$Repository/releases?per_page=100"
    if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_LIST_FAILED'}
    $pages=$raw|Out-String|ConvertFrom-Json
    $found=@(foreach($page in $pages){foreach($entry in $page){if($entry.tag_name -eq $tag){$entry}}})
    if($found.Count -gt 1){throw 'DRAFT_ARTIFACT_AMBIGUOUS'}
    if($found.Count -eq 1){return $found[0]}
    return $null
}
$release=Find-ArtifactDraft
if(-not $release){
    $notes=Join-Path $BundleDirectory release-notes.txt
    [IO.File]::WriteAllText($notes,"Private draft build input. Source $SourceCommit. ELF $ExpectedServerSha256. Do not publish this release.",[Text.UTF8Encoding]::new($false))
    & gh release create $tag --repo $Repository --draft --target $SourceCommit --title "MRFPS server artifact $($hash.Substring(0,12))" --notes-file $notes
    if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_CREATE_FAILED'}
    $release=Find-ArtifactDraft
}
if(-not $release -or -not $release.draft){throw 'ARTIFACT_RELEASE_MUST_REMAIN_PRIVATE_DRAFT'}
$existing=@($release.assets|Where-Object {$_.name -eq 'linux-server.tar.gz'})
if($existing.Count){
    if($existing.Count -ne 1 -or $existing[0].digest -ne "sha256:$hash"){throw 'EXISTING_ARTIFACT_DIGEST_MISMATCH_OR_UNVERIFIED'}
}else{
    & gh release upload $tag $archive --repo $Repository
    if($LASTEXITCODE -ne 0){throw 'DRAFT_ARTIFACT_UPLOAD_FAILED'}
}
& gh workflow run publish-linux-server.yml --repo $Repository --ref main -f "source_commit=$SourceCommit" -f "release_tag=$tag" -f "bundle_sha256=$hash" -f "server_sha256=$ExpectedServerSha256"
if($LASTEXITCODE -ne 0){throw 'GHCR_WORKFLOW_DISPATCH_FAILED'}
$plan.workflowDispatched=$true
$plan|ConvertTo-Json|Set-Content (Join-Path $BundleDirectory publication-dispatch.json) -Encoding UTF8
$plan|ConvertTo-Json
