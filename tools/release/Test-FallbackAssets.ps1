param(
    [Parameter(Mandatory)][string]$AssetRoot,
    [Parameter(Mandatory)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
$toolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $toolRoot 'FallbackAssets.ps1')
$allowlist = Join-Path $toolRoot 'windows-fallback-allowlist.json'
$source = (Resolve-Path -LiteralPath $AssetRoot).Path
$test = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
if (Test-Path -LiteralPath $test) { throw 'TestRoot must be a fresh path.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $test))) { throw 'TestRoot parent missing.' }

$positive = Get-ReviewedFallbackAssets -SourceRoot $source -AllowlistPath $allowlist
if (@($positive.runtime_files).Count -ne 30) { throw 'Positive fallback coverage incomplete.' }
Write-Output 'PASS: exact 34-file source and 30-file runtime allowlist.'

Copy-Item -LiteralPath $source -Destination $test -Recurse
function Expect-Rejected([string]$label,[string]$pattern) {
    try {
        Get-ReviewedFallbackAssets -SourceRoot $test -AllowlistPath $allowlist | Out-Null
        throw "Negative fixture was accepted: $label"
    } catch {
        if ($_.Exception.Message -notmatch $pattern) { throw }
        Write-Output "PASS rejected $label`: $($_.Exception.Message)"
    }
}
$private = Join-Path $test 'textures\private-card-art.jpg'
[IO.File]::WriteAllBytes($private, [byte[]]@(1,2,3))
Expect-Rejected 'unlisted private card art' 'Unreviewed fallback source file count'
Remove-Item -LiteralPath $private

$manifest = Join-Path $test 'SOURCE_MANIFEST.json'
[IO.File]::AppendAllText($manifest, "`n")
Expect-Rejected 'modified source manifest' 'Fallback source manifest differs from reviewed pin'
Copy-Item -LiteralPath (Join-Path $source 'SOURCE_MANIFEST.json') -Destination $manifest -Force

$cover = Join-Path $test 'textures\cover.png'
[IO.File]::AppendAllText($cover, 'x')
Expect-Rejected 'tampered cover bytes' 'Fallback source file differs from manifest: textures/cover.png'
Copy-Item -LiteralPath (Join-Path $source 'textures\cover.png') -Destination $cover -Force

$missing = Join-Path $test 'textures\target.png'
Remove-Item -LiteralPath $missing
Expect-Rejected 'missing required sprite' 'Unreviewed fallback source file count'
Write-Output "Retained disposable negative fixture: $test"
