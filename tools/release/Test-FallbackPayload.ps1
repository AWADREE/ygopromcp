param(
    [Parameter(Mandatory)][string]$AssetRoot,
    [Parameter(Mandatory)][string]$BaselinePackageDirectory,
    [Parameter(Mandatory)][string]$Objdump,
    [Parameter(Mandatory)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $tools 'FallbackAssets.ps1')
$assets = (Resolve-Path -LiteralPath $AssetRoot).Path
$baseline = (Resolve-Path -LiteralPath $BaselinePackageDirectory).Path
$test = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
if (Test-Path -LiteralPath $test) { throw 'TestRoot must be new.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $test))) { throw 'TestRoot parent missing.' }
$allowlist = Get-ReviewedFallbackAssets -SourceRoot $assets `
    -AllowlistPath (Join-Path $tools 'windows-fallback-allowlist.json')

Copy-Item -LiteralPath $baseline -Destination $test -Recurse
$manifestPath = Join-Path $test 'MANIFEST.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest | Add-Member -NotePropertyName fallback_source_manifest_sha256 `
    -NotePropertyValue $allowlist.source_manifest_sha256
$manifest | Add-Member -NotePropertyName fallback_asset_commit `
    -NotePropertyValue $allowlist.reviewed_asset_commit
$additions = @()
foreach ($asset in $allowlist.runtime_files) {
    $from = Join-Path $assets ([string]$asset.source_path).Replace('/','\')
    $relative = [string]$asset.destination_path
    $to = Join-Path $test $relative.Replace('/','\')
    New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
    Copy-Item -LiteralPath $from -Destination $to
    $additions += [pscustomobject]@{path=$relative;size=$asset.size;sha256=$asset.sha256}
}
$manifest.files = @($manifest.files) + $additions
$positiveJson = (($manifest | ConvertTo-Json -Depth 8) + "`n")
[IO.File]::WriteAllText($manifestPath, $positiveJson, [Text.UTF8Encoding]::new($false))

$zipPath = "$test.zip"
$stream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
    try {
        $prefix = [IO.Path]::GetFileName($test)
        foreach ($file in @(Get-ChildItem -LiteralPath $test -Recurse -File | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($test.Length + 1).Replace('\','/')
            $entry = $zip.CreateEntry("$prefix/$relative",[IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(1980,1,1,0,0,0,[TimeSpan]::Zero)
            $input = [IO.File]::OpenRead($file.FullName)
            try {
                $output = $entry.Open()
                try { $input.CopyTo($output) } finally { $output.Dispose() }
            } finally { $input.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }

$check = Join-Path $tools 'Test-WindowsPayload.ps1'
$result = & $check -PackageDirectory $test -Objdump $Objdump
if (($result -join ' ') -notmatch '^PASS: 2 executable hashes, 5 recursive DLL imports') {
    throw 'Reviewed fallback payload was not accepted.'
}
Write-Output "PASS reviewed 30-file fallback payload: $result"
Write-Output "Positive ZIP SHA256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash)"

$withoutAssets = "$test-omission"
if (Test-Path -LiteralPath $withoutAssets) { throw 'Omission fixture path already exists.' }
Copy-Item -LiteralPath $test -Destination $withoutAssets -Recurse
foreach ($asset in $allowlist.runtime_files) {
    $path = Join-Path $withoutAssets ([string]$asset.destination_path).Replace('/','\')
    Remove-Item -LiteralPath $path
}
$withoutManifestPath = Join-Path $withoutAssets 'MANIFEST.json'
$withoutManifest = Get-Content -LiteralPath $withoutManifestPath -Raw | ConvertFrom-Json
$withoutManifest.PSObject.Properties.Remove('fallback_source_manifest_sha256')
$withoutManifest.PSObject.Properties.Remove('fallback_asset_commit')
$assetPaths = @($allowlist.runtime_files | ForEach-Object { [string]$_.destination_path })
$withoutManifest.files = @($withoutManifest.files | Where-Object { $_.path -notin $assetPaths })
[IO.File]::WriteAllText($withoutManifestPath,
    (($withoutManifest | ConvertTo-Json -Depth 8) + "`n"),[Text.UTF8Encoding]::new($false))
try {
    & $check -PackageDirectory $withoutAssets -Objdump $Objdump | Out-Null
    throw 'Package without fallback pin and all fallback assets was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Package fallback source manifest differs from reviewed pin') {
        throw
    }
    Write-Output "PASS rejected omitted pin and all 30 assets: $($_.Exception.Message)"
}

$private = Join-Path $test 'textures\private-card-art.jpg'
[IO.File]::WriteAllBytes($private,[byte[]]@(1,2,3))
$manifest.files = @($manifest.files) + [pscustomobject]@{
    path='textures/private-card-art.jpg';size=3;
    sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $private).Hash
}
[IO.File]::WriteAllText($manifestPath,(($manifest | ConvertTo-Json -Depth 8) + "`n"),
    [Text.UTF8Encoding]::new($false))
try {
    & $check -PackageDirectory $test -Objdump $Objdump | Out-Null
    throw 'Unlisted private card art was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Unreviewed fallback asset in package: textures/private-card-art.jpg') {
        throw
    }
    Write-Output "PASS rejected unlisted private package art: $($_.Exception.Message)"
}
Write-Output "Retained positive ZIP and disposable negative extracted fixture: $test"
