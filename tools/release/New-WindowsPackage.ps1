param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$LauncherExecutable,
    [Parameter(Mandatory)][string]$BuildReceipt,
    [Parameter(Mandatory)][string]$DllDirectory,
    [Parameter(Mandatory)][string]$Objdump,
    [Parameter(Mandatory)][string]$LicenseDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$BuildLog
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression

$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
$launcher = (Resolve-Path -LiteralPath $LauncherExecutable).Path
$dllRoot = (Resolve-Path -LiteralPath $DllDirectory).Path
$dump = (Resolve-Path -LiteralPath $Objdump).Path
$licenseRoot = (Resolve-Path -LiteralPath $LicenseDirectory).Path
$receipt = Get-Content -LiteralPath $BuildReceipt -Raw | ConvertFrom-Json
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$zipPath = "$output.zip"
if ($output.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package output must be outside the source tree.'
}
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $zipPath)) {
    throw 'Package output or ZIP already exists; use a new destination.'
}
if (-not (Test-Path -LiteralPath (Split-Path -Parent $output))) { throw 'Output parent does not exist.' }
if ($receipt.schema_version -ne 1 -or $receipt.toolchain -ne 'Windows GNU/UCRT64') { throw 'Wrong build receipt format/toolchain.' }
$audioProfile = if ($receipt.audio_profile) { $receipt.audio_profile } else { 'no-audio' }
if ($audioProfile -notin @('no-audio','miniaudio-opus-vorbis')) { throw 'Unknown audio profile in build receipt.' }
if ($audioProfile -eq 'miniaudio-opus-vorbis') {
    if (-not $BuildLog -or -not $receipt.build_log_sha256) { throw 'Full audio requires its saved build log and hash.' }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Resolve-Path -LiteralPath $BuildLog).Path).Hash -ne $receipt.build_log_sha256) {
        throw 'Full-audio build log does not match the receipt.'
    }
}
$headOutput = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or -not $headOutput) { throw 'Cannot resolve source commit. Check repository access.' }
$head = $headOutput.Trim()
if ($head -ne $receipt.source_commit) { throw 'Build receipt source commit differs from checkout.' }
if ((& git -C $source status --porcelain --untracked-files=no)) { throw 'Tracked source changes remain uncommitted.' }
$exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
if ($exeHash -ne $receipt.executable_sha256 -or (Get-Item -LiteralPath $exe).Length -ne $receipt.executable_size) {
    throw 'Executable does not match build receipt.'
}
if ([IO.Path]::GetFileName($exe) -ne 'YGOPro.exe') { throw 'Expected YGOPro.exe.' }
$launcherHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $launcher).Hash
if ([IO.Path]::GetFileName($launcher) -ne 'YGOProUpdateLauncher.exe' -or
    [IO.Path]::GetDirectoryName($exe) -ine [IO.Path]::GetDirectoryName($launcher) -or
    [IO.Path]::GetDirectoryName($exe) -ine (Join-Path $source 'bin\release') -or
    $launcherHash -ne $receipt.launcher_executable_sha256 -or
    (Get-Item -LiteralPath $launcher).Length -ne $receipt.launcher_executable_size) {
    throw 'Native update launcher does not match the same-build receipt.'
}

$toolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$helperConfig = Get-Content -LiteralPath (Join-Path $toolRoot 'windows-native-helper-allowlist.json') -Raw | ConvertFrom-Json
if ($helperConfig.schema_version -ne 1) { throw 'Unreviewed native helper allowlist.' }
$helperNames = @($helperConfig.helpers | ForEach-Object { [string]$_.name })
if (@($helperNames | Sort-Object -Unique).Count -ne $helperNames.Count -or
    @($receipt.native_helpers).Count -ne $helperNames.Count) {
    throw 'Native helper receipt differs from reviewed allowlist.'
}
$helpers = @(
    foreach ($name in $helperNames) {
        if ($name -notmatch '^YGOPro[A-Za-z0-9-]+\.exe$' -or $name -in @('YGOPro.exe','YGOProUpdateLauncher.exe')) {
            throw "Unreviewed native helper name: $name"
        }
        $path = Join-Path $source "bin\release\$name"
        $pin = @($receipt.native_helpers | Where-Object { $_.name -ceq $name })
        if ($pin.Count -ne 1 -or -not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $pin[0].sha256 -or
            (Get-Item -LiteralPath $path).Length -ne $pin[0].size) {
            throw "Native helper differs from same-build receipt: $name"
        }
        [pscustomobject]@{name=$name;path=$path;sha256=$pin[0].sha256;size=$pin[0].size}
    }
)
$entries = @(Get-Content -LiteralPath (Join-Path $toolRoot 'windows-ucrt64-dependencies.json') -Raw | ConvertFrom-Json)
$noticeEntries = @(Get-Content -LiteralPath (Join-Path $toolRoot 'windows-ucrt64-license-files.json') -Raw | ConvertFrom-Json)
$staticNotices = @()
if ($audioProfile -eq 'miniaudio-opus-vorbis') {
    $staticNotices = @(Get-Content -LiteralPath (Join-Path $toolRoot 'windows-static-audio-notices.json') -Raw | ConvertFrom-Json)
    $staticRoot = Join-Path $toolRoot 'static-audio-notices'
    if ($staticNotices.Count -ne 5 -or -not (Test-Path -LiteralPath $staticRoot -PathType Container)) {
        throw 'Full-audio notice allowlist is incomplete.'
    }
    $expectedComponents = @('miniaudio','ogg','opus','opusfile','vorbis')
    if (@(Compare-Object @($staticNotices.component | Sort-Object) $expectedComponents).Count) {
        throw 'Unexpected static-audio notice components.'
    }
    if (@($staticNotices | Where-Object { $_.file -notin @('LICENSE','COPYING') }).Count) {
        throw 'Invalid static-audio notice filename.'
    }
    $expectedNoticePaths = @($staticNotices | ForEach-Object { "$($_.component)\$($_.file)" })
    if (@($expectedNoticePaths | Sort-Object -Unique).Count -ne 5) { throw 'Duplicate static-audio notice.' }
    $staticItems = @(Get-ChildItem -LiteralPath $staticRoot -Recurse -Force)
    if (@($staticItems | Where-Object { -not $_.PSIsContainer }).Count -ne 5) { throw 'Unexpected static-audio notice count.' }
    foreach ($item in $staticItems) {
        $relative = $item.FullName.Substring($staticRoot.Length + 1)
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Static-audio notice reparse point: $relative" }
        if ($item.PSIsContainer) {
            if ($relative -notin $staticNotices.component) { throw "Unreviewed static-audio notice directory: $relative" }
        } elseif ($relative -notin $expectedNoticePaths) { throw "Unreviewed static-audio notice: $relative" }
    }
    foreach ($entry in $staticNotices) {
        $path = Join-Path $staticRoot "$($entry.component)\$($entry.file)"
        if ($entry.sha256 -notmatch '^[0-9a-f]{64}$' -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $entry.sha256) {
            throw "Static-audio notice hash mismatch: $path"
        }
    }
}
$allow = @{}
$noticeFiles = @{}
foreach ($entry in $entries) {
    if ($allow.ContainsKey($entry.dll)) { throw "Duplicate DLL mapping: $($entry.dll)" }
    $allow[$entry.dll] = $entry.license
}
foreach ($entry in $noticeEntries) {
    if ($noticeFiles.ContainsKey($entry.package)) { throw "Duplicate license package mapping: $($entry.package)" }
    $noticeFiles[$entry.package] = @($entry.files)
}
$system = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
@('ADVAPI32','BCRYPT','COMCTL32','COMDLG32','CRYPT32','DNSAPI','DWMAPI','DWRITE',
  'GDI32','IMM32','IPHLPAPI','KERNEL32','MSIMG32','MSVCRT','NTDLL','OLE32',
  'OLEAUT32','OPENGL32','RPCRT4','SECUR32','SETUPAPI','SHELL32','SHLWAPI',
  'UCRTBASE','USER32','USP10','VERSION','WINHTTP','WINMM','WS2_32') | ForEach-Object { [void]$system.Add("$_.dll") }
$queue = [Collections.Generic.Queue[string]]::new()
$queue.Enqueue($exe)
$queue.Enqueue($launcher)
foreach ($helper in $helpers) { $queue.Enqueue($helper.path) }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$required = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$oldPath = $env:PATH
try {
    # objdump itself is a UCRT64 executable; this changes this process only.
    $env:PATH = "$dllRoot;$oldPath"
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        if (-not $seen.Add([IO.Path]::GetFileName($file))) { continue }
        $format = & $dump -f $file
        if ($LASTEXITCODE -ne 0 -or ($format -join "`n") -notmatch 'file format pei-x86-64') {
            throw "Expected x64 PE binary: $file"
        }
        $imports = & $dump -p $file
        if ($LASTEXITCODE -ne 0) { throw "objdump failed for $file" }
        foreach ($line in $imports) {
            if ($line -notmatch 'DLL Name:\s*(\S+)') { continue }
            $name = $Matches[1]
            if ($name -match '^(api-ms-|ext-ms-)' -or $system.Contains($name)) { continue }
            if (-not $allow.ContainsKey($name)) { throw "Unreviewed DLL import $name in $file" }
            $dep = Join-Path $dllRoot $name
            if (-not (Test-Path -LiteralPath $dep -PathType Leaf)) { throw "Missing runtime DLL: $dep" }
            $required[$name] = $allow[$name]
            $queue.Enqueue($dep)
        }
    }
} finally { $env:PATH = $oldPath }

foreach ($package in @($required.Values | Sort-Object -Unique)) {
    $dir = Join-Path $licenseRoot $package
    if (-not $noticeFiles.ContainsKey($package) -or -not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw "Missing reviewed license mapping/directory: $package"
    }
    $actual = @(Get-ChildItem -LiteralPath $dir -Recurse -Force)
    $actualFiles = @($actual | Where-Object { -not $_.PSIsContainer })
    if ($actualFiles.Count -ne $noticeFiles[$package].Count) { throw "Unexpected license files in $dir" }
    foreach ($file in $actual) {
        $relative = $file.FullName.Substring($dir.Length + 1)
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in license path: $relative" }
        if ($file.PSIsContainer) {
            if (-not @($noticeFiles[$package] | Where-Object { $_.StartsWith($relative + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) {
                throw "Unreviewed license directory: $relative"
            }
        } elseif ($relative -notin $noticeFiles[$package]) { throw "Unreviewed license path: $relative" }
    }
}
$gilded = Join-Path $source 'themes\gilded'
foreach ($path in @('LICENSE','themes\gilded\theme.json','themes\gilded\assets\backdrop.png','themes\gilded\assets\logo.png')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $path) -PathType Leaf)) { throw "Missing reviewed source file: $path" }
}
$themeAllowed = @('theme.json', 'assets\backdrop.png', 'assets\logo.png')
$themeItems = @(Get-ChildItem -LiteralPath $gilded -Recurse -Force)
foreach ($item in $themeItems) {
    $relative = $item.FullName.Substring($gilded.Length + 1)
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        ($item.PSIsContainer -and $relative -ne 'assets') -or
        (-not $item.PSIsContainer -and $relative -notin $themeAllowed)) {
        throw "Unreviewed Gilded theme path: $relative"
    }
}
if (@($themeItems | Where-Object { -not $_.PSIsContainer }).Count -ne $themeAllowed.Count) {
    throw 'Gilded theme allowlist is incomplete.'
}
. (Join-Path $toolRoot 'FallbackAssets.ps1')
$fallbackRoot = Join-Path $source 'tools\release\fallback'
$fallback = Get-ReviewedFallbackAssets -SourceRoot $fallbackRoot `
    -AllowlistPath (Join-Path $toolRoot 'windows-fallback-allowlist.json')
New-Item -ItemType Directory -Path $output | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $output 'YGOPro.exe')
Copy-Item -LiteralPath $launcher -Destination (Join-Path $output 'YGOProUpdateLauncher.exe')
foreach ($helper in $helpers) { Copy-Item -LiteralPath $helper.path -Destination (Join-Path $output $helper.name) }
foreach ($name in @($required.Keys | Sort-Object)) {
    Copy-Item -LiteralPath (Join-Path $dllRoot $name) -Destination (Join-Path $output $name)
}
foreach ($name in @('Play YGOPro.cmd','MCP YGOPro.cmd','Check game data.cmd','GAME-DATA-SETUP.txt')) {
    Copy-Item -LiteralPath (Join-Path $toolRoot $name) -Destination (Join-Path $output $name)
}
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $output 'LICENSE')
New-Item -ItemType Directory -Path (Join-Path $output 'themes') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output 'themes\gilded\assets') -Force | Out-Null
foreach ($relative in $themeAllowed) {
    Copy-Item -LiteralPath (Join-Path $gilded $relative) -Destination (Join-Path $output "themes\gilded\$relative")
}
New-Item -ItemType Directory -Path (Join-Path $output 'THIRD_PARTY_LICENSES') | Out-Null
foreach ($asset in $fallback.runtime_files) {
    $from = Join-Path $fallbackRoot ([string]$asset.source_path).Replace('/','\')
    $to = Join-Path $output ([string]$asset.destination_path).Replace('/','\')
    New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
    Copy-Item -LiteralPath $from -Destination $to
}
foreach ($package in @($required.Values | Sort-Object -Unique)) {
    $target = Join-Path $output "THIRD_PARTY_LICENSES\$package"
    New-Item -ItemType Directory -Path $target | Out-Null
    foreach ($file in $noticeFiles[$package]) {
        $destination = Join-Path $target $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Join-Path $licenseRoot $package) $file) -Destination $destination
    }
}

if ($audioProfile -eq 'miniaudio-opus-vorbis') {
    foreach ($entry in $staticNotices) {
        $target = Join-Path $output "THIRD_PARTY_LICENSES\static-audio\$($entry.component)"
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $staticRoot "$($entry.component)\$($entry.file)") -Destination (Join-Path $target $entry.file)
    }
}

$files = @(Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($output.Length + 1).Replace('\','/')
        size = $_.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
    }
})
$manifest = [ordered]@{
    schema_version = 1
    release_status = 'draft; corresponding source, asset provenance, and final build review pending'
    source_commit = $head
    executable_sha256 = $exeHash
    launcher_executable_sha256 = $launcherHash
    launcher_executable_size = (Get-Item -LiteralPath $launcher).Length
    native_helpers = @($helpers | ForEach-Object { [ordered]@{name=$_.name;sha256=$_.sha256;size=$_.size} })
    audio_profile = $audioProfile
    build_log_sha256 = $receipt.build_log_sha256
    static_audio_notices = @($staticNotices | ForEach-Object {
        [ordered]@{ component = $_.component; file = $_.file; sha256 = $_.sha256 }
    })
    distribution = 'Windows GNU/UCRT64 program, original fallback UI and Gilded theme; card data excluded'
    fallback_source_manifest_sha256 = $fallback.source_manifest_sha256
    fallback_asset_commit = $fallback.reviewed_asset_commit
    excluded = @('cards.cdb','strings.conf','personal card art','script','deck','replay','system.conf','logs','diagnostics','transcripts')
    dlls = @($required.Keys | Sort-Object)
    dll_license_packages = @($required.Keys | Sort-Object | ForEach-Object {
        [ordered]@{ dll = $_; license_package = $required[$_] }
    })
    files = $files
}
[IO.File]::WriteAllText((Join-Path $output 'MANIFEST.json'),
    (($manifest | ConvertTo-Json -Depth 8) + "`n"), [Text.UTF8Encoding]::new($false))

# Stable file order and timestamps make repeated packaging with identical
# inputs yield the same ZIP bytes. The manifest intentionally excludes itself.
$stream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $prefix = [IO.Path]::GetFileName($output)
        foreach ($file in @(Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($output.Length + 1).Replace('\','/')
            $entry = $zip.CreateEntry("$prefix/$relative", [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(1980,1,1,0,0,0,[TimeSpan]::Zero)
            $inputStream = [IO.File]::OpenRead($file.FullName)
            try { $entryStream = $entry.Open(); try { $inputStream.CopyTo($entryStream) } finally { $entryStream.Dispose() } }
            finally { $inputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }
Write-Output "Package: $output"
Write-Output "ZIP: $zipPath"
Write-Output "ZIP SHA256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash)"
Write-Output "Bundled DLLs: $($required.Count); bundled files: $($files.Count + 1)."
