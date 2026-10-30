[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BundleDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedServerSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [string]$Repository='mizuamedesu/MRFPS-Server-Registry',
    [string]$SourceRepository='mizuamedesu/MRFPS',
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
if(-not $RunnerLabel){throw 'PRIVATE_RUNNER_REQUIRED'}
$identity=Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../tools/cloud/private-registry-identity.json') -Raw|ConvertFrom-Json
$package=(& gh api 'users/mizuamedesu/packages/container/mrfps-server-private'|Out-String)|ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $identity.repository -ne $Repository -or $publisher.id -ne $identity.publisher_repository_id -or
   $package.id -ne $identity.package_id -or $package.created_at -ne $identity.created_at -or
   $package.name -ne $identity.package -or $package.visibility -ne 'private' -or $package.package_type -ne 'container'){
    throw 'PRIVATE_PACKAGE_IDENTITY_OR_VISIBILITY_MISMATCH'
}
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
