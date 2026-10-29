[CmdletBinding()]
param([string]$Repository='mizuamedesu/MRFPS',[string]$ReportDirectory,[ValidatePattern('^mrfps-privacy-probe-[a-f0-9]{16}$')][string]$ExistingTag)
$ErrorActionPreference='Stop'
if($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$'){throw 'REPOSITORY_INVALID'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $ReportDirectory){$ReportDirectory=Join-Path $root ('Saved/Validation/CloudPrivacy-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))}
$ReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
if(Test-Path -LiteralPath $ReportDirectory){throw 'PRIVACY_REPORT_ALREADY_EXISTS'}
New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
$tag=if($ExistingTag){$ExistingTag}else{'mrfps-privacy-probe-'+[Guid]::NewGuid().ToString('N').Substring(0,16)}
$fixture=Join-Path $ReportDirectory privacy-fixture.txt
[IO.File]::WriteAllText($fixture,"MRFPS non-secret draft-access fixture`n",[Text.UTF8Encoding]::new($false))
$notes=Join-Path $ReportDirectory notes.txt
[IO.File]::WriteAllText($notes,'Non-secret fixture only. Do not publish this draft. No game binaries or credentials.',[Text.UTF8Encoding]::new($false))
if(-not $ExistingTag){
    & gh release create $tag --repo $Repository --draft --target main --title 'MRFPS private artifact access probe' --notes-file $notes
    if($LASTEXITCODE -ne 0){throw 'DRAFT_FIXTURE_CREATE_FAILED'}
}
function Get-Draft {
    # GitHub's by-tag endpoint does not return unpublished drafts, even to an
    # authenticated owner. List is permission-filtered and includes own drafts.
    for($attempt=0;$attempt -lt 8;$attempt++){
        $raw=& gh api --paginate --slurp "repos/$Repository/releases?per_page=100"
        if($LASTEXITCODE -ne 0){throw 'AUTHENTICATED_DRAFT_READ_FAILED'}
        $pages=$raw|Out-String|ConvertFrom-Json
        $releases=@(foreach($page in $pages){foreach($entry in $page){if($entry.tag_name -eq $tag){$entry}}})
        if($releases.Count -gt 1 -or ($releases.Count -eq 1 -and -not $releases[0].draft)){throw 'FIXTURE_MUST_REMAIN_DRAFT'}
        if($releases.Count -eq 1){return $releases[0]}
        if($attempt -lt 7){Start-Sleep -Milliseconds 750}
    }
    throw 'DRAFT_FIXTURE_NOT_VISIBLE_AFTER_BOUNDED_RETRY'
}
function Get-AnonymousStatus([string]$Url,[string]$Accept='application/json'){
    try{$response=Invoke-WebRequest -UseBasicParsing -Uri $Url -Headers @{Accept=$Accept} -MaximumRedirection 0 -TimeoutSec 20; return [int]$response.StatusCode}
    catch{if($_.Exception.Response){return [int]$_.Exception.Response.StatusCode};throw 'ANONYMOUS_PRIVACY_REQUEST_FAILED'}
}
$draft=Get-Draft
$metadataStatus=Get-AnonymousStatus "https://api.github.com/repos/$Repository/releases/$($draft.id)"
if($metadataStatus -ne 404){throw 'ANONYMOUS_DRAFT_METADATA_NOT_DENIED'}
& gh release upload $tag $fixture --repo $Repository
if($LASTEXITCODE -ne 0){throw 'DRAFT_FIXTURE_UPLOAD_FAILED'}
for($attempt=0;$attempt -lt 8;$attempt++){
    $draft=Get-Draft
    $asset=@($draft.assets|Where-Object {$_.name -eq 'privacy-fixture.txt'})
    if($asset.Count -ne 0){break}
    if($attempt -lt 7){Start-Sleep -Milliseconds 750}
}
if($asset.Count -ne 1){throw 'DRAFT_FIXTURE_ASSET_MISSING'}
$assetStatus=Get-AnonymousStatus $asset[0].browser_download_url 'application/octet-stream'
$apiStatus=Get-AnonymousStatus $asset[0].url 'application/octet-stream'
$report=[ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');repository=$Repository;releaseTag=$tag;releaseId=$draft.id;fixtureAssetId=$asset[0].id;authenticatedDraft=$true;anonymousMetadataStatus=$metadataStatus;anonymousAssetStatus=$assetStatus;anonymousAssetApiStatus=$apiStatus;anonymousDenied=($assetStatus -eq 404 -and $apiStatus -eq 404);realServerUploaded=$false;credentialsUploaded=$false;actionsTokenCheck='pending workflow commit and dispatch';draftPublished=$false}
$report|ConvertTo-Json|Set-Content (Join-Path $ReportDirectory report.json) -Encoding UTF8
$report|ConvertTo-Json
if(-not $report.anonymousDenied){throw 'ANONYMOUS_DRAFT_ASSET_NOT_DENIED'}
