param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$LauncherExecutable,
    [Parameter(Mandatory)][string]$OutputFile,
    [ValidateSet('no-audio','miniaudio-opus-vorbis')][string]$AudioProfile = 'no-audio',
    [string]$BuildLog
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
$launcher = (Resolve-Path -LiteralPath $LauncherExecutable).Path
if ([IO.Path]::GetFileName($exe) -ne 'YGOPro.exe' -or
    [IO.Path]::GetFileName($launcher) -ne 'YGOProUpdateLauncher.exe' -or
    [IO.Path]::GetDirectoryName($exe) -ine [IO.Path]::GetDirectoryName($launcher) -or
    [IO.Path]::GetDirectoryName($exe) -ine (Join-Path $root 'bin\release')) {
    throw 'Game and update launcher must be named outputs of this Release build.'
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'AGENTS.md'))) { throw 'Not a YGOPro source root.' }
$headOutput = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or -not $headOutput) { throw 'Cannot resolve source commit. Check repository access.' }
$head = $headOutput.Trim()
if ($head -notmatch '^[0-9a-f]{40}$') { throw 'Invalid source commit.' }
if ((& git -C $root status --porcelain --untracked-files=no)) { throw 'Tracked source changes remain uncommitted.' }
if (Test-Path -LiteralPath $OutputFile) { throw 'Receipt output already exists.' }
if ($AudioProfile -ne 'no-audio' -and -not $BuildLog) { throw 'Full audio requires the saved build log.' }
$helperConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-native-helper-allowlist.json') -Raw | ConvertFrom-Json
if ($helperConfig.schema_version -ne 1) { throw 'Unreviewed native helper allowlist.' }
$helperNames = @($helperConfig.helpers | ForEach-Object { [string]$_.name })
if (@($helperNames | Sort-Object -Unique).Count -ne $helperNames.Count) { throw 'Duplicate native helper name.' }
$helpers = @(
    foreach ($name in $helperNames) {
        if ($name -notmatch '^YGOPro[A-Za-z0-9-]+\.exe$' -or $name -eq 'YGOProUpdateLauncher.exe' -or $name -eq 'YGOPro.exe') {
            throw "Unreviewed native helper name: $name"
        }
        $path = Join-Path $root "bin\release\$name"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Native helper build output is missing or redirected: $name"
        }
        [ordered]@{name=$name;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;size=(Get-Item -LiteralPath $path).Length}
    }
)
$buildLogHash = if ($BuildLog) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath (Resolve-Path -LiteralPath $BuildLog).Path).Hash
} else { $null }
$receipt = [ordered]@{
    schema_version = 1
    source_commit = $head
    executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    executable_size = (Get-Item -LiteralPath $exe).Length
    launcher_executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $launcher).Hash
    launcher_executable_size = (Get-Item -LiteralPath $launcher).Length
    native_helpers = $helpers
    toolchain = 'Windows GNU/UCRT64'
    audio_profile = $AudioProfile
    build_log_sha256 = $buildLogHash
    note = 'Hash and source revision recorded after build; this receipt alone does not prove build provenance.'
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputFile),
    (($receipt | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
Write-Output "Recorded $head and executable hash $($receipt.executable_sha256)."
