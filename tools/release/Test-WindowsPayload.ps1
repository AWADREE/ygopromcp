param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$Objdump
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$dir = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\')
$dump = (Resolve-Path -LiteralPath $Objdump).Path
$zipPath = "$dir.zip"
$manifestPath = Join-Path $dir 'MANIFEST.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$fallbackPin = '08DCD76F3FF2568F76A0718B7265ADDB0370DB1DEAEC7714DADF40A7061B9F93'
$fallbackFiles = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
if ($manifest.fallback_source_manifest_sha256 -ne $fallbackPin) {
    throw 'Package fallback source manifest differs from reviewed pin.'
}
if ($manifest.fallback_asset_commit -ne '933cb20f8fda10f1cb6ebd1e0f2f49fd4d564762') {
    throw 'Package fallback asset revision differs from reviewed pin.'
}
$toolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$allowlist = Get-Content -LiteralPath (Join-Path $toolRoot 'windows-fallback-allowlist.json') -Raw | ConvertFrom-Json
if ($allowlist.schema_version -ne 1 -or $allowlist.source_manifest_sha256 -ne $fallbackPin -or
    @($allowlist.runtime_files).Count -ne 30) {
    throw 'Package fallback allowlist differs from review.'
}
foreach ($asset in $allowlist.runtime_files) {
    $name = [string]$asset.destination_path
    if ($fallbackFiles.ContainsKey($name)) { throw "Duplicate fallback path: $name" }
    $fallbackFiles.Add($name, $asset)
}
if ($manifest.schema_version -ne 1 -or
    $manifest.executable_sha256 -notmatch '^[0-9a-f]{64}$' -or
    $manifest.launcher_executable_sha256 -notmatch '^[0-9a-f]{64}$' -or
    $manifest.launcher_executable_size -le 0) {
    throw 'Package manifest does not pin both executable builds.'
}

$expected = @{}
foreach ($file in @($manifest.files)) {
    $name = [string]$file.path
    if ($name -notmatch '^[A-Za-z0-9_.+ /-]+$' -or
        $name -match '(^|/)\.\.?(?:/|$)' -or $expected.ContainsKey($name)) {
        throw "Invalid or duplicate package path: $name"
    }
    if ($name -match '^(cards\.cdb|strings\.conf|system\.conf|script/|deck/|replay/|logs/|diagnostics/|transcripts/|card-updates/)') {
        throw "Private game data in package: $name"
    }
    if ($name -match '^(textures/|fonts/|THIRD_PARTY_LICENSES/NotoSansCJK/)') {
        if (-not $fallbackFiles.ContainsKey($name) -or
            $fallbackFiles[$name].sha256 -ne $file.sha256 -or
            $fallbackFiles[$name].size -ne $file.size) {
            throw "Unreviewed fallback asset in package: $name"
        }
    }
    $expected[$name] = $file
    $path = Join-Path $dir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -ne $file.size -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Package file differs from manifest: $name"
    }
}
foreach ($name in $fallbackFiles.Keys) {
    if (-not $expected.ContainsKey($name)) { throw "Reviewed fallback asset omitted from package: $name" }
}
foreach ($name in @('themes/gilded/theme.json','themes/gilded/assets/backdrop.png','themes/gilded/assets/logo.png')) {
    if (-not $expected.ContainsKey($name)) { throw "Gilded theme asset omitted from package: $name" }
}
foreach ($name in @('YGOPro.exe','YGOProUpdateLauncher.exe')) {
    if (-not $expected.ContainsKey($name)) { throw "Executable omitted from manifest: $name" }
}
$helperNames = @()
$helperEntries = if ($null -eq $manifest.native_helpers) { @() } else { @($manifest.native_helpers) }
foreach ($helper in $helperEntries) {
    $name = [string]$helper.name
    if ($name -notmatch '^YGOPro[A-Za-z0-9-]+\.exe$' -or
        $name -in @('YGOPro.exe','YGOProUpdateLauncher.exe') -or
        $name -in $helperNames -or -not $expected.ContainsKey($name) -or
        $expected[$name].sha256 -ne $helper.sha256 -or
        $expected[$name].size -ne $helper.size) {
        throw "Native helper omitted or differs from manifest: $name"
    }
    $helperNames += $name
}
$knownExecutables = @('YGOPro.exe','YGOProUpdateLauncher.exe') + $helperNames
foreach ($name in @($expected.Keys | Where-Object { $_ -match '\.exe$' })) {
    if ($name -notin $knownExecutables) { throw "Unreviewed executable in package: $name" }
}
if ($expected['YGOPro.exe'].sha256 -ne $manifest.executable_sha256 -or
    $expected['YGOProUpdateLauncher.exe'].sha256 -ne $manifest.launcher_executable_sha256 -or
    $expected['YGOProUpdateLauncher.exe'].size -ne $manifest.launcher_executable_size) {
    throw 'Executable hashes differ from manifest build pins.'
}
$actual = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Force)
if ($actual.Count -ne $expected.Count + 1 -or
    @($actual | Where-Object { $_.FullName.Substring($dir.Length + 1).Replace('\','/') -ne 'MANIFEST.json' -and
        -not $expected.ContainsKey($_.FullName.Substring($dir.Length + 1).Replace('\','/')) }).Count) {
    throw 'Unmanifested package file.'
}
$listedDlls = @($manifest.dlls)
$actualDlls = @(Get-ChildItem -LiteralPath $dir -File -Filter '*.dll' | ForEach-Object Name)
if (@(Compare-Object @($listedDlls | Sort-Object) @($actualDlls | Sort-Object)).Count -or
    @($listedDlls | Sort-Object -Unique).Count -ne $listedDlls.Count) {
    throw 'Manifest DLL inventory differs from payload.'
}

$zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    if ($zip.Entries.Count -ne $expected.Count + 1) { throw 'ZIP entry count differs from manifest.' }
    $seen = @{}
    $prefix = [IO.Path]::GetFileName($dir) + '/'
    foreach ($entry in $zip.Entries) {
        if (-not $entry.FullName.StartsWith($prefix,[StringComparison]::Ordinal) -or
            $seen.ContainsKey($entry.FullName)) { throw "Unexpected ZIP path: $($entry.FullName)" }
        $seen[$entry.FullName] = $true
        $name = $entry.FullName.Substring($prefix.Length)
        if ($name -eq 'MANIFEST.json') {
            $known = Get-Item -LiteralPath $manifestPath
            $bytes = $known.Length
            $hash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
        } elseif ($expected.ContainsKey($name)) {
            $bytes = $expected[$name].size
            $hash = $expected[$name].sha256
        } else { throw "Unmanifested ZIP path: $name" }
        $stream = $entry.Open()
        try { $actualHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $stream.Dispose() }
        if ($entry.Length -ne $bytes -or $actualHash -ne $hash) {
            throw "ZIP payload differs from manifest: $name"
        }
    }
} finally { $sha.Dispose(); $zip.Dispose() }

$system = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
@('ADVAPI32','BCRYPT','COMCTL32','COMDLG32','CRYPT32','DNSAPI','DWMAPI','DWRITE',
  'GDI32','IMM32','IPHLPAPI','KERNEL32','MSIMG32','MSVCRT','NTDLL','OLE32',
  'OLEAUT32','OPENGL32','RPCRT4','SECUR32','SETUPAPI','SHELL32','SHLWAPI',
  'UCRTBASE','USER32','USP10','VERSION','WINHTTP','WINMM','WS2_32') |
    ForEach-Object { [void]$system.Add("$_.dll") }
$queue = [Collections.Generic.Queue[string]]::new()
foreach ($name in @('YGOPro.exe','YGOProUpdateLauncher.exe')) { $queue.Enqueue($name) }
foreach ($name in $helperNames) { $queue.Enqueue($name) }
$seenImports = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$closure = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$priorPath = $env:PATH
try {
    # objdump is UCRT64-linked; this changes only this test process.
    $env:PATH = "$(Split-Path -Parent $dump);$priorPath"
    while ($queue.Count) {
        $name = $queue.Dequeue()
        if (-not $seenImports.Add($name)) { continue }
        $path = Join-Path $dir $name
        $format = & $dump -f $path
        if ($LASTEXITCODE -ne 0 -or ($format -join "`n") -notmatch 'file format pei-x86-64') {
            throw "Expected x64 PE binary: $name"
        }
        $imports = & $dump -p $path
        if ($LASTEXITCODE -ne 0) { throw "objdump failed for $name" }
        foreach ($line in $imports) {
            if ($line -notmatch 'DLL Name:\s*(\S+)') { continue }
            $dependency = $Matches[1]
            if ($dependency -match '^(api-ms-|ext-ms-)' -or $system.Contains($dependency)) { continue }
            if (-not $expected.ContainsKey($dependency) -or
                -not (Test-Path -LiteralPath (Join-Path $dir $dependency) -PathType Leaf)) {
                throw "Missing packaged DLL import $dependency required by $name"
            }
            [void]$closure.Add($dependency)
            $queue.Enqueue($dependency)
        }
    }
} finally { $env:PATH = $priorPath }
if (@(Compare-Object @($closure | Sort-Object) @($listedDlls | Sort-Object)).Count) {
    throw 'Manifest DLL list differs from recursive executable closure.'
}
Write-Output "PASS: $($helperNames.Count + 2) executable hashes, $($closure.Count) recursive DLL imports, every extracted file and every ZIP entry match."
