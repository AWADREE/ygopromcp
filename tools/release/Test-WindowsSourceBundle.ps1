param(
    [Parameter(Mandatory)][string]$BundleDirectory,
    [string]$Executable,
    [string]$LauncherExecutable
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$dir = (Resolve-Path -LiteralPath $BundleDirectory).Path.TrimEnd('\')
$zipPath = "$dir.zip"
$manifest = Get-Content -LiteralPath (Join-Path $dir 'SOURCE-MANIFEST.json') -Raw | ConvertFrom-Json
$receipt = Get-Content -LiteralPath (Join-Path $dir 'build\build-receipt.json') -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or $manifest.audio_profile -ne 'miniaudio-opus-vorbis' -or
    @($manifest.files).Count -lt 70 -or $manifest.ucrt64_package_count -ne 18 -or
    $manifest.ucrt64_notice_package_count -ne 18) { throw 'Unexpected source manifest.' }
if ($Executable -and ((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -ne $manifest.executable_sha256 -or
    (Get-Item -LiteralPath $Executable).Length -ne $manifest.executable_size)) {
    throw 'Executable differs from the source manifest.'
}
if ($LauncherExecutable -and ((Get-FileHash -LiteralPath $LauncherExecutable -Algorithm SHA256).Hash -ne $manifest.launcher_executable_sha256 -or
    (Get-Item -LiteralPath $LauncherExecutable).Length -ne $manifest.launcher_executable_size)) {
    throw 'Native launcher differs from the source manifest.'
}
if ($manifest.launcher_executable_sha256 -and
    ($manifest.launcher_executable_sha256 -ne $receipt.launcher_executable_sha256 -or
     $manifest.launcher_executable_size -ne $receipt.launcher_executable_size -or
     @($manifest.native_helpers).Count -ne @($receipt.native_helpers).Count)) {
    throw 'Source manifest differs from its native executable build receipt.'
}
foreach ($helper in @($manifest.native_helpers | Where-Object { $null -ne $_ })) {
    $pin = @($receipt.native_helpers | Where-Object { $_.name -ceq $helper.name })
    if ($pin.Count -ne 1 -or $pin[0].sha256 -ne $helper.sha256 -or $pin[0].size -ne $helper.size) {
        throw "Native helper source receipt mismatch: $($helper.name)"
    }
}
$expected = @{}
foreach ($file in $manifest.files) {
    $path = [string]$file.path
    if ($path -notmatch '^[A-Za-z0-9_.+/-]+$' -or $path -match '(^|/)\.\.?(?:/|$)' -or $expected.ContainsKey($path)) {
        throw "Invalid or duplicate source path: $path"
    }
    $expected[$path] = $file
    $local = Join-Path $dir $path
    if (-not (Test-Path -LiteralPath $local -PathType Leaf) -or
        (Get-Item -LiteralPath $local).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Bundle directory file mismatch: $path"
    }
}
if (@($expected.Keys | Where-Object { $_ -like 'source/ucrt64/*' }).Count -ne 17 -or
    @($expected.Keys | Where-Object { $_ -like 'source/audio/*' }).Count -ne 4 -or
    @($expected.Keys | Where-Object { $_ -like 'notices/ucrt64/*' }).Count -ne 36 -or
    @($expected.Keys | Where-Object { $_ -like 'notices/*' }).Count -ne 41) {
    throw 'Source or notice coverage is incomplete.'
}
$actualFiles = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Force)
if ($actualFiles.Count -ne $expected.Count + 1) { throw 'Unmanifested file in source bundle directory.' }
$zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    if ($zip.Entries.Count -ne $expected.Count + 1) { throw 'Unexpected outer ZIP entry count.' }
    $seen = @{}
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\','/')
        if ($seen.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
        $seen[$name] = $true
        if ($name -eq 'SOURCE-MANIFEST.json') {
            $knownHash = (Get-FileHash -LiteralPath (Join-Path $dir $name) -Algorithm SHA256).Hash
            $knownBytes = (Get-Item -LiteralPath (Join-Path $dir $name)).Length
        } elseif ($expected.ContainsKey($name)) {
            $knownHash = $expected[$name].sha256
            $knownBytes = $expected[$name].bytes
        } else { throw "Unmanifested ZIP entry: $name" }
        if ($entry.Length -ne $knownBytes) { throw "ZIP entry size mismatch: $name" }
        $stream = $entry.Open()
        try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)) }
        finally { $stream.Dispose() }
        if ($hash -ne $knownHash) { throw "ZIP entry hash mismatch: $name" }
    }
} finally { $sha.Dispose(); $zip.Dispose() }

$required = @{
    'ygopromcp.zip' = @('LICENSE','gframe/game.cpp','premake5.lua')
    'ocgcore.zip' = @('LICENSE','processor.cpp')
    'miniaudio.zip' = @('LICENSE','extras/miniaudio_split/miniaudio.c')
    'modified-irrlicht.zip' = @('doc/irrlicht-license.txt','include/irrlicht.h')
}
foreach ($archive in $required.Keys) {
    $inner = [IO.Compression.ZipFile]::OpenRead((Join-Path $dir "source/$archive"))
    try {
        $names = @($inner.Entries | ForEach-Object FullName)
        foreach ($name in $required[$archive]) {
            if ($name -notin $names) { throw "Missing source or license in ${archive}: $name" }
        }
    } finally { $inner.Dispose() }
}
Write-Output "PASS: $($expected.Count) source/notice files and all outer ZIP bytes match; 17 UCRT64 sources, 18 notice packages, 4 audio sources, and nested licenses present."
Write-Output "ZIP SHA256: $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
