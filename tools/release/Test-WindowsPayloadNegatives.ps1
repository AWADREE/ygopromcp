param(
    [Parameter(Mandatory)][string]$LauncherExecutable,
    [Parameter(Mandatory)][string]$DllDirectory,
    [Parameter(Mandatory)][string]$AssetRoot,
    [Parameter(Mandatory)][string]$Objdump,
    [Parameter(Mandatory)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$launcher = (Resolve-Path -LiteralPath $LauncherExecutable).Path
$dllRoot = (Resolve-Path -LiteralPath $DllDirectory).Path
$dump = (Resolve-Path -LiteralPath $Objdump).Path
$assetRoot = (Resolve-Path -LiteralPath $AssetRoot).Path
$toolRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $toolRoot 'FallbackAssets.ps1')
$fallback = Get-ReviewedFallbackAssets -SourceRoot $assetRoot `
    -AllowlistPath (Join-Path $toolRoot 'windows-fallback-allowlist.json')
$root = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
if ((Test-Path -LiteralPath $root) -or
    -not (Test-Path -LiteralPath (Split-Path -Parent $root))) {
    throw 'Use a new test directory under an existing parent.'
}
if ([IO.Path]::GetFileName($launcher) -ne 'YGOProUpdateLauncher.exe') {
    throw 'Expected the built native update launcher.'
}
$fakeGame = Join-Path $env:SystemRoot 'System32\cmd.exe'
if (-not (Test-Path -LiteralPath $fakeGame)) { throw 'Windows test PE is missing.' }
$launcherDlls = @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll',
    'libsqlite3-0.dll','zlib1.dll')
foreach ($name in $launcherDlls) {
    if (-not (Test-Path -LiteralPath (Join-Path $dllRoot $name))) {
        throw "Missing pinned launcher test DLL: $name"
    }
}
$check = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'Test-WindowsPayload.ps1'
New-Item -ItemType Directory -Path $root | Out-Null

function New-Fixture([string]$name,[string]$missingDll,[bool]$omitLauncherRecord,
    [bool]$helperMode = $false,[bool]$omitHelperRecord = $false,[bool]$extraExecutable = $false) {
    $dir = Join-Path $root $name
    New-Item -ItemType Directory -Path $dir | Out-Null
    Copy-Item -LiteralPath $fakeGame -Destination (Join-Path $dir 'YGOPro.exe')
    if ($helperMode) {
        Copy-Item -LiteralPath $fakeGame -Destination (Join-Path $dir 'YGOProUpdateLauncher.exe')
        Copy-Item -LiteralPath $launcher -Destination (Join-Path $dir 'YGOProNativeUpdater.exe')
    } else {
        Copy-Item -LiteralPath $launcher -Destination (Join-Path $dir 'YGOProUpdateLauncher.exe')
    }
    if ($extraExecutable) { Copy-Item -LiteralPath $fakeGame -Destination (Join-Path $dir 'unreviewed.exe') }
    foreach ($dll in $launcherDlls) {
        if ($dll -ne $missingDll) {
            Copy-Item -LiteralPath (Join-Path $dllRoot $dll) -Destination (Join-Path $dir $dll)
        }
    }
    foreach ($asset in $fallback.runtime_files) {
        $from = Join-Path $assetRoot ([string]$asset.source_path).Replace('/','\')
        $to = Join-Path $dir ([string]$asset.destination_path).Replace('/','\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
        Copy-Item -LiteralPath $from -Destination $to
    }
    $files = @(Get-ChildItem -LiteralPath $dir -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($dir.Length + 1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    if ($omitLauncherRecord) {
        $files = @($files | Where-Object { $_.path -ne 'YGOProUpdateLauncher.exe' })
    }
    if ($omitHelperRecord) {
        $files = @($files | Where-Object { $_.path -ne 'YGOProNativeUpdater.exe' })
    }
    $helperPins = if ($helperMode) {
        @([ordered]@{
            name='YGOProNativeUpdater.exe'
            sha256=(Get-FileHash -LiteralPath (Join-Path $dir 'YGOProNativeUpdater.exe') -Algorithm SHA256).Hash
            size=(Get-Item -LiteralPath (Join-Path $dir 'YGOProNativeUpdater.exe')).Length
        })
    } else { @() }
    $manifest = [ordered]@{
        schema_version=1
        fallback_source_manifest_sha256=$fallback.source_manifest_sha256
        fallback_asset_commit=$fallback.reviewed_asset_commit
        executable_sha256=(Get-FileHash -LiteralPath (Join-Path $dir 'YGOPro.exe') -Algorithm SHA256).Hash
        launcher_executable_sha256=(Get-FileHash -LiteralPath (Join-Path $dir 'YGOProUpdateLauncher.exe') -Algorithm SHA256).Hash
        launcher_executable_size=(Get-Item -LiteralPath (Join-Path $dir 'YGOProUpdateLauncher.exe')).Length
        native_helpers=$helperPins
        dlls=@(Get-ChildItem -LiteralPath $dir -File -Filter '*.dll' | Sort-Object Name | ForEach-Object Name)
        files=$files
    }
    [IO.File]::WriteAllText((Join-Path $dir 'MANIFEST.json'),
        (($manifest | ConvertTo-Json -Depth 8) + "`n"),[Text.UTF8Encoding]::new($false))
    $stream = [IO.File]::Open("$dir.zip",[IO.FileMode]::CreateNew)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
        try {
            foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -Recurse | Sort-Object FullName)) {
                $relative = $file.FullName.Substring($dir.Length + 1).Replace('\','/')
                $entry = $archive.CreateEntry("$name/$relative",[IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(1980,1,1,0,0,0,[TimeSpan]::Zero)
                $input = [IO.File]::OpenRead($file.FullName)
                try { $output = $entry.Open(); try { $input.CopyTo($output) } finally { $output.Dispose() } }
                finally { $input.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
    return $dir
}

$positive = New-Fixture 'positive' '' $false
$success = & $check -PackageDirectory $positive -Objdump $dump
if (($success -join ' ') -notmatch '^PASS: 2 executable hashes, 5 recursive DLL imports') {
    throw 'Static positive fixture did not prove launcher import closure.'
}
$missing = New-Fixture 'missing-launcher-dll' 'libstdc++-6.dll' $false
try {
    & $check -PackageDirectory $missing -Objdump $dump | Out-Null
    throw 'Missing launcher DLL was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Missing packaged DLL import libstdc\+\+-6\.dll required by YGOProUpdateLauncher\.exe') {
        throw
    }
    $missingError = $_.Exception.Message
}
$omitted = New-Fixture 'omitted-launcher-manifest' '' $true
try {
    & $check -PackageDirectory $omitted -Objdump $dump | Out-Null
    throw 'Unmanifested launcher was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Executable omitted from manifest: YGOProUpdateLauncher\.exe') {
        throw
    }
    $omittedError = $_.Exception.Message
}
$helperPositive = New-Fixture 'helper-positive' '' $false $true
$helperResult = & $check -PackageDirectory $helperPositive -Objdump $dump
if (($helperResult -join ' ') -notmatch '^PASS: 3 executable hashes, 5 recursive DLL imports') {
    throw 'Static native-helper fixture did not prove its import closure.'
}
$helperMissing = New-Fixture 'missing-helper-dll' 'libstdc++-6.dll' $false $true
try {
    & $check -PackageDirectory $helperMissing -Objdump $dump | Out-Null
    throw 'Missing native-helper DLL was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Missing packaged DLL import libstdc\+\+-6\.dll required by YGOProNativeUpdater\.exe') {
        throw
    }
    $helperMissingError = $_.Exception.Message
}
$helperOmitted = New-Fixture 'omitted-helper-manifest' '' $false $true $true
try {
    & $check -PackageDirectory $helperOmitted -Objdump $dump | Out-Null
    throw 'Unmanifested native helper was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Native helper omitted or differs from manifest: YGOProNativeUpdater\.exe') {
        throw
    }
    $helperOmittedError = $_.Exception.Message
}
$extra = New-Fixture 'extra-executable' '' $false $true $false $true
try {
    & $check -PackageDirectory $extra -Objdump $dump | Out-Null
    throw 'Unreviewed packaged executable was accepted.'
} catch {
    if ($_.Exception.Message -notmatch 'Unreviewed executable in package: unreviewed\.exe') { throw }
    $extraError = $_.Exception.Message
}
Write-Output "PASS: $success"
Write-Output "PASS expected missing launcher DLL: $missingError"
Write-Output "PASS expected manifest omission: $omittedError"
Write-Output "PASS: $helperResult"
Write-Output "PASS expected missing native-helper DLL: $helperMissingError"
Write-Output "PASS expected native-helper manifest omission: $helperOmittedError"
Write-Output "PASS expected unreviewed executable: $extraError"
Write-Output "Fixture directory: $root"
