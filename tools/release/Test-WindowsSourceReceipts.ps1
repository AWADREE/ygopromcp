param([Parameter(Mandatory)][string]$ArchiveDirectory)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $ArchiveDirectory).Path
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$receipt = Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-source-receipts.json') -Raw | ConvertFrom-Json
$dllMap = @(Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-dependencies.json') -Raw | ConvertFrom-Json)
if ($receipt.schema_version -ne 1 -or @($receipt.archives).Count -ne 17) {
    throw 'Unexpected source receipt format or archive count.'
}
$names = @($receipt.archives | ForEach-Object name)
if (@($names | Sort-Object -Unique).Count -ne $names.Count) { throw 'Duplicate source archive.' }
$actual = @(Get-ChildItem -LiteralPath $root -File -Force)
if ($actual.Count -ne $names.Count -or @($actual | Where-Object Name -notin $names).Count) {
    throw 'Source archive folder has missing or unreviewed files.'
}
$packages = @()
foreach ($entry in $receipt.archives) {
    if ($entry.name -notmatch '^mingw-w64-[A-Za-z0-9.+_-]+\.src\.tar\.zst$' -or
        $entry.url -ne "https://mirror.msys2.org/mingw/sources/$($entry.name)" -or
        $entry.sha256 -notmatch '^[0-9a-f]{64}$' -or
        $entry.size -lt 1) {
        throw "Invalid source receipt: $($entry.name)"
    }
    $file = Get-Item -LiteralPath (Join-Path $root $entry.name)
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point: $($entry.name)" }
    if ($file.Length -ne $entry.size -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash -ne $entry.sha256) {
        throw "Source hash or size mismatch: $($entry.name)"
    }
    $packages += @($entry.installed_packages)
}
if (@($packages | Sort-Object -Unique).Count -ne 18) { throw 'Expected 18 unique installed packages.' }
foreach ($license in @($dllMap.license | Sort-Object -Unique)) {
    $prefix = "mingw-w64-ucrt-x86_64-$license-"
    if (@($packages | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) }).Count -ne 1) {
        throw "Missing or ambiguous installed source receipt: $license"
    }
}
Write-Output "PASS: $($names.Count) hashed official source archives cover 18 installed UCRT64 packages."
