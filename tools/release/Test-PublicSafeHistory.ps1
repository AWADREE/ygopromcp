param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$BaseRef,
    [Parameter(Mandatory)][string]$CandidateRef
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Repository).Path
function Resolve-Commit([string]$ref) {
    $sha = (& git -C $root rev-parse --verify "$ref^{commit}").Trim()
    if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-f]{40}$') { throw "Invalid Git ref: $ref" }
    return $sha
}
$base = Resolve-Commit $BaseRef
$candidate = Resolve-Commit $CandidateRef
$ancestor = (& git -C $root merge-base $base $candidate).Trim()
if ($LASTEXITCODE -ne 0 -or $ancestor -ne $base) { throw 'Base must be an ancestor of candidate.' }
$range = "$base..$candidate"
$commits = @(& git -C $root rev-list $range)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate candidate commits.' }
$objects = @(& git -C $root rev-list --objects $range)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate newly reachable objects.' }

$privateCardBackBlob = '7cc3153695f89a965297469a7f430de7b4f19221'
$approvedIconPath = 'resource/gframe/ygopro.ico'
$approvedIconBlob = '11ce3202cb3b0758abf83e947c6eccc4b1dcc238'
$reviewedImagesFile = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'windows-reviewed-public-images.json'
if ((Get-FileHash -LiteralPath $reviewedImagesFile -Algorithm SHA256).Hash -ne
    'B05FDF1739D7255B4C1B2612BE92C18E454BF615A6BEA7C764D767ADB2B0B19B') {
    throw 'Reviewed public image allowlist changed.'
}
$imageReview = Get-Content -LiteralPath $reviewedImagesFile -Raw | ConvertFrom-Json
if ($imageReview.schema_version -ne 1 -or
    $imageReview.reviewed_fallback_commit -ne '933cb20f8fda10f1cb6ebd1e0f2f49fd4d564762' -or
    @($imageReview.images).Count -ne 30) {
    throw 'Reviewed public image inventory differs from approval.'
}
$allowedImages = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
foreach ($entry in $imageReview.images) {
    if ($entry.path -notmatch '^(tools/release/fallback/textures/[A-Za-z0-9_.+-]+|themes/gilded/assets/(logo|backdrop)\.png|docs/assets/ygopromcp-banner\.png)$' -or
        $entry.git_blob -notmatch '^[0-9a-f]{40}$' -or
        $allowedImages.ContainsKey([string]$entry.path)) {
        throw "Invalid reviewed image entry: $($entry.path)"
    }
    $allowedImages.Add([string]$entry.path, [string]$entry.git_blob)
}
$bad = @()
foreach ($line in $objects) {
    if ($line -notmatch '^([0-9a-f]{40})(?: (.*))?$') { throw "Unexpected rev-list object: $line" }
    $sha = $Matches[1]
    $path = $Matches[2]
    if ($sha -eq $privateCardBackBlob) { $bad += "private card-back blob $sha" }
    if (-not $path) { continue }
    $path = $path.Replace('\', '/')
    if ($path -match '(^|/)(cards\.cdb|strings\.conf|system\.conf|deck|decks|replay|replays|logs|diagnostics|transcripts)(/|$)' -or
        $path -match '^textures/cover2?\.png$' -or
        $path -match '\.(dmp|mdmp)$') {
        $bad += "private path $path"
    }
    if ($path -match '\.(png|jpe?g|webp|gif)$' -and
        (-not $allowedImages.ContainsKey($path) -or $sha -ne $allowedImages[$path])) {
        $bad += "unreviewed image $path $sha"
    }
    if ($path -match '\.ico$' -and
        ($path -cne $approvedIconPath -or $sha -ne $approvedIconBlob)) {
        $bad += "unreviewed icon $path $sha"
    }
}
$candidateIcon = (& git -C $root rev-parse "$candidate`:$approvedIconPath").Trim()
if ($LASTEXITCODE -ne 0 -or $candidateIcon -ne $approvedIconBlob) {
    $bad += 'Approved Windows icon differs from candidate source.'
}
if ($bad.Count) { throw "Unsafe newly reachable history:`n$($bad -join "`n")" }
Write-Output "PASS: $($commits.Count) new commits and $($objects.Count) reachable objects exclude known private data and unreviewed images."
