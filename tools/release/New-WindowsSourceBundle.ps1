param(
    [Parameter(Mandatory)][string]$ClientRepository,
    [Parameter(Mandatory)][string]$ClientCommit,
    [Parameter(Mandatory)][string]$PublicBaseCommit,
    [Parameter(Mandatory)][string]$OcgcoreRepository,
    [Parameter(Mandatory)][string]$MiniaudioRepository,
    [Parameter(Mandatory)][string]$IrrlichtArchive,
    [Parameter(Mandatory)][string]$LuaArchive,
    [Parameter(Mandatory)][string]$AudioArchiveDirectory,
    [Parameter(Mandatory)][string]$Ucrt64ArchiveDirectory,
    [Parameter(Mandatory)][string]$LicenseDirectory,
    [Parameter(Mandatory)][string]$BuildReceipt,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$LauncherExecutable,
    [Parameter(Mandatory)][string]$BuildLog,
    [Parameter(Mandatory)][string]$PremakeLog,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression

$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$client = (Resolve-Path -LiteralPath $ClientRepository).Path
$clientHead = (& git -C $client rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $clientHead -ne $ClientCommit -or
    (& git -C $client status --porcelain --untracked-files=no)) {
    throw 'Client checkout is not the exact clean source commit.'
}
$ocgcore = (Resolve-Path -LiteralPath $OcgcoreRepository).Path
$miniaudio = (Resolve-Path -LiteralPath $MiniaudioRepository).Path
$out = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$zip = "$out.zip"
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $zip)) {
    throw 'Source bundle output already exists; choose a fresh destination.'
}
if (-not (Test-Path -LiteralPath (Split-Path -Parent $out))) { throw 'Output parent is missing.' }
if ($ClientCommit -notmatch '^[0-9a-f]{40}$' -or $PublicBaseCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Client and base revisions must be full Git commit IDs.'
}
$receipt = Get-Content -LiteralPath $BuildReceipt -Raw | ConvertFrom-Json
if ($receipt.source_commit -ne $ClientCommit -or $receipt.audio_profile -ne 'miniaudio-opus-vorbis' -or
    $receipt.executable_sha256 -ne (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -or
    $receipt.executable_size -ne (Get-Item -LiteralPath $Executable).Length -or
    $receipt.launcher_executable_sha256 -ne (Get-FileHash -LiteralPath $LauncherExecutable -Algorithm SHA256).Hash -or
    $receipt.launcher_executable_size -ne (Get-Item -LiteralPath $LauncherExecutable).Length -or
    $receipt.build_log_sha256 -ne (Get-FileHash -LiteralPath $BuildLog -Algorithm SHA256).Hash) {
    throw 'The executable/build-log receipt does not match the selected client revision.'
}
if ([IO.Path]::GetFileName($Executable) -ne 'YGOPro.exe' -or
    [IO.Path]::GetFileName($LauncherExecutable) -ne 'YGOProUpdateLauncher.exe' -or
    [IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $Executable).Path) -ine (Join-Path $client 'bin\release') -or
    [IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $LauncherExecutable).Path) -ine (Join-Path $client 'bin\release')) {
    throw 'Executable paths do not belong to the selected client Release build.'
}
$helperConfig = Get-Content -LiteralPath (Join-Path $tools 'windows-native-helper-allowlist.json') -Raw | ConvertFrom-Json
if ($helperConfig.schema_version -ne 1) { throw 'Unreviewed native helper allowlist.' }
$helperNames = @($helperConfig.helpers | ForEach-Object { [string]$_.name })
if (@($helperNames | Sort-Object -Unique).Count -ne $helperNames.Count -or
    @($receipt.native_helpers).Count -ne $helperNames.Count) {
    throw 'Native helper source receipt differs from reviewed allowlist.'
}
$helperPins = @(
    foreach ($name in $helperNames) {
        if ($name -notmatch '^YGOPro[A-Za-z0-9-]+\.exe$' -or $name -in @('YGOPro.exe','YGOProUpdateLauncher.exe')) {
            throw "Unreviewed native helper name: $name"
        }
        $pin = @($receipt.native_helpers | Where-Object { $_.name -ceq $name })
        $path = Join-Path $client "bin\release\$name"
        if ($pin.Count -ne 1 -or -not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $pin[0].sha256 -or
            (Get-Item -LiteralPath $path).Length -ne $pin[0].size) {
            throw "Native helper source receipt mismatch: $name"
        }
        [ordered]@{name=$name;sha256=$pin[0].sha256;size=$pin[0].size}
    }
)
& (Join-Path $tools 'Test-PublicSafeHistory.ps1') -Repository $client -BaseRef $PublicBaseCommit -CandidateRef $ClientCommit | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Public-safe history audit failed.' }

$coreGitlink = @(& git -C $client ls-tree $ClientCommit -- ocgcore)
if ($LASTEXITCODE -ne 0 -or $coreGitlink.Count -ne 1) {
    throw 'Client commit lacks a valid ocgcore Gitlink.'
}
$coreMatch = [regex]::Match($coreGitlink[0], '^160000 commit ([0-9a-f]{40})\s+ocgcore$')
if (-not $coreMatch.Success) { throw 'Client commit lacks a valid ocgcore Gitlink.' }
$coreRevision = $coreMatch.Groups[1].Value
if ($coreRevision -notin @(
    'ee5c2ec7d6acfb583bfb7e26e6ee667db46677fa',
    '3209cd866a55e8501a0ed358d3064d9ae0ae6b30')) {
    throw "Unreviewed ocgcore revision in client commit: $coreRevision"
}
$pins = [ordered]@{
    ocgcore = $coreRevision
    miniaudio = '9634bedb5b5a2ca38c1ee7108a9358a4e233f14d'
    irrlicht_sha256 = '2d7d5d48c03b01497b2a7c2478af93fdc66fa853a178d17c5818373cd6cda7b4'
    lua_sha256 = '4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae'
}
$audio = [ordered]@{
    'libogg-1.3.6.tar.gz' = '83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638'
    'opus-1.5.2.tar.gz' = '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'
    'opusfile-0.12.tar.gz' = '118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b'
    'libvorbis-1.3.7.tar.gz' = '0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab'
}
$ucrtReceipts = Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-source-receipts.json') -Raw | ConvertFrom-Json
$ucrtNotices = @(Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-license-files.json') -Raw | ConvertFrom-Json)
& (Join-Path $tools 'Test-WindowsSourceReceipts.ps1') -ArchiveDirectory $Ucrt64ArchiveDirectory | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'UCRT64 source receipts failed.' }
if ($ucrtNotices.Count -ne 18) { throw 'Unexpected UCRT64 notice map.' }

function Assert-GitCommit([string]$repository, [string]$expected) {
    $actual = (& git -C $repository rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) { throw "Wrong source revision: $repository" }
    if ((& git -C $repository status --porcelain --untracked-files=no)) {
        throw "Modified tracked source: $repository"
    }
}
Assert-GitCommit $ocgcore $pins.ocgcore
Assert-GitCommit $miniaudio $pins.miniaudio

$files = [Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Path $out | Out-Null
function Record-File([string]$relative, [string]$origin, [string]$expectedHash) {
    $path = Join-Path $out $relative
    $item = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expectedHash -and $hash -ne $expectedHash.ToLowerInvariant()) {
        throw "Source hash mismatch: $relative"
    }
    $script:files.Add([ordered]@{ path = $relative.Replace('\','/'); bytes = $item.Length;
        sha256 = $hash; origin = $origin })
}
function Copy-Pinned([string]$from, [string]$relative, [string]$origin, [string]$hash) {
    $target = Join-Path $out $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $from -Destination $target
    Record-File $relative $origin $hash
}
function Archive-Git([string]$repo, [string]$revision, [string]$relative, [string]$origin) {
    $target = Join-Path $out $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    & git -C $repo archive --format=zip --output=$target $revision
    if ($LASTEXITCODE -ne 0) { throw "Git source export failed: $origin" }
    Record-File $relative $origin ''
}

Archive-Git $client $ClientCommit 'source/ygopromcp.zip' "public-safe client commit $ClientCommit"
Archive-Git $ocgcore $pins.ocgcore 'source/ocgcore.zip' "ocgcore commit $($pins.ocgcore)"
Archive-Git $miniaudio $pins.miniaudio 'source/miniaudio.zip' "miniaudio commit $($pins.miniaudio)"
Copy-Pinned $IrrlichtArchive 'source/modified-irrlicht.zip' 'modified Irrlicht commit 0c7734b570e7e7ab8a846e1965e851b4a2014562' $pins.irrlicht_sha256
Copy-Pinned $LuaArchive 'source/lua-5.4.8.tar.gz' 'official Lua 5.4.8 release' $pins.lua_sha256
foreach ($name in $audio.Keys) {
    Copy-Pinned (Join-Path $AudioArchiveDirectory $name) "source/audio/$name" 'official Xiph release; hash pinned in client CI' $audio[$name]
}
foreach ($entry in $ucrtReceipts.archives) {
    Copy-Pinned (Join-Path $Ucrt64ArchiveDirectory $entry.name) "source/ucrt64/$($entry.name)" $entry.url $entry.sha256
}
foreach ($entry in $ucrtNotices) {
    if ($entry.package -notmatch '^[a-z0-9+-]+$') { throw 'Invalid UCRT64 notice package.' }
    foreach ($file in @($entry.files)) {
        if ($file -notmatch '^[A-Za-z0-9._\\-]+$' -or $file -match '(^|\\)\.\.?(\\|$)') {
            throw 'Invalid UCRT64 notice path.'
        }
        $relative = "notices/ucrt64/$($entry.package)/$file"
        Copy-Pinned (Join-Path (Join-Path $LicenseDirectory $entry.package) $file) $relative 'installed UCRT64 dependency notice' ''
    }
}
$notices = @(Get-Content -LiteralPath (Join-Path $tools 'windows-static-audio-notices.json') -Raw | ConvertFrom-Json)
foreach ($notice in $notices) {
    $relative = "notices/$($notice.component)/$($notice.file)"
    Copy-Pinned (Join-Path $tools "static-audio-notices/$($notice.component)/$($notice.file)") $relative 'source-matched static audio notice' $notice.sha256
}
Copy-Pinned $BuildLog 'build/build.log' 'observed full-audio build log' $receipt.build_log_sha256
Copy-Pinned $PremakeLog 'build/premake.log' 'observed Premake command and output' ''
Copy-Pinned $BuildReceipt 'build/build-receipt.json' 'observed executable/source/build-log hashes' ''

$manifest = [ordered]@{
    schema_version = 1
    status = 'draft-source-for-observed-audio-probe; not final integrated release'
    client_commit = $ClientCommit
    public_base_commit = $PublicBaseCommit
    executable_sha256 = $receipt.executable_sha256.ToLowerInvariant()
    executable_size = $receipt.executable_size
    launcher_executable_sha256 = $receipt.launcher_executable_sha256.ToLowerInvariant()
    launcher_executable_size = $receipt.launcher_executable_size
    native_helpers = $helperPins
    audio_profile = $receipt.audio_profile
    pinned_revisions = $pins
    ucrt64_package_count = 18
    ucrt64_notice_package_count = $ucrtNotices.Count
    files = @($files)
    limitations = @('The observed executable embeds an ignored private icon/resource absent from this source bundle.',
        'A final binary requires approved original resource input and a new exact build/source bundle.',
        'The client archive contains Gitlinks; only build-used ocgcore and modified Irrlicht submodule sources are separately included.',
        'Local card databases, images, scripts, decks, settings, replays and private logs are excluded.')
}
$manifestPath = Join-Path $out 'SOURCE-MANIFEST.json'
[IO.File]::WriteAllText($manifestPath, (($manifest | ConvertTo-Json -Depth 12) + "`n"), [Text.UTF8Encoding]::new($false))
[IO.Compression.ZipFile]::CreateFromDirectory($out, $zip, [IO.Compression.CompressionLevel]::NoCompression, $false)
Write-Output "DRAFT source bundle: $zip"
Write-Output "Files: $($files.Count); bundle SHA256: $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"
