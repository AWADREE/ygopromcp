param([Parameter(Mandatory)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
if (Test-Path -LiteralPath $root) { throw 'TestRoot must not already exist.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $root))) { throw 'TestRoot parent does not exist.' }
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$entries = @(Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-dependencies.json') -Raw | ConvertFrom-Json)
$notices = @(Get-Content -LiteralPath (Join-Path $tools 'windows-ucrt64-license-files.json') -Raw | ConvertFrom-Json)
$audioNotices = @(Get-Content -LiteralPath (Join-Path $tools 'windows-static-audio-notices.json') -Raw | ConvertFrom-Json)
if ($entries.Count -lt 19) { throw 'Expected reviewed UCRT64 dependency map.' }
$names = @($entries | ForEach-Object { $_.dll.ToLowerInvariant() })
if (@($names | Sort-Object -Unique).Count -ne $names.Count) { throw 'Duplicate DLL mapping.' }
if (@($notices.package | Sort-Object -Unique).Count -ne $notices.Count) { throw 'Duplicate notice package.' }
foreach ($entry in $entries) {
    if ($entry.dll -notmatch '^[A-Za-z0-9_.+-]+\.dll$' -or $entry.license -notmatch '^[A-Za-z0-9_.+-]+$') {
        throw 'Invalid DLL/license mapping.'
    }
    if ($entry.license -notin $notices.package) { throw "Missing notice allowlist for $($entry.dll)" }
}
foreach ($notice in $notices) {
    if (-not $notice.files -or @($notice.files | Sort-Object -Unique).Count -ne @($notice.files).Count) {
        throw "Invalid notice file allowlist for $($notice.package)"
    }
}
if ($audioNotices.Count -ne 5 -or @($audioNotices.component | Sort-Object -Unique).Count -ne 5) {
    throw 'Expected five reviewed static-audio notices.'
}
foreach ($notice in $audioNotices) {
    if ($notice.component -notin @('miniaudio','ogg','opus','opusfile','vorbis') -or
        $notice.file -notin @('LICENSE','COPYING') -or
        $notice.sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid static-audio notice map.' }
    $path = Join-Path $tools "static-audio-notices\$($notice.component)\$($notice.file)"
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $notice.sha256) {
        throw "Static-audio notice hash mismatch: $($notice.component)"
    }
}
New-Item -ItemType Directory -Path $root | Out-Null
Copy-Item -LiteralPath (Join-Path $tools 'Check game data.cmd') -Destination (Join-Path $root 'Check game data.cmd')
$check = Join-Path $root 'Check game data.cmd'
$missingOutput = & $check 2>&1
if ($LASTEXITCODE -ne 1 -or ($missingOutput -join ' ') -notmatch 'game data is incomplete') {
    throw 'Missing-game-data preflight did not fail with guidance.'
}
$uiTextures = @('act.png','attack.png','bg.jpg','bg_deck.jpg','bg_menu.jpg',
    'chain.png','chaintarget.png','equip.png','f1.jpg','f2.jpg','f3.jpg',
    'field.png','field2.png','field3.png','field-transparent2.png',
    'field-transparent3.png','lim.png','lp.png','lpf.png','mask.png',
    'negated.png','number.png','ot.png','target.png','unknown.jpg',
    'cover.jpg','cover2.jpg')
$required = @('cards.cdb','strings.conf','script\constant.lua') +
    @($uiTextures | ForEach-Object { "textures\$_" })
foreach ($relative in $required) {
    $path = Join-Path $root $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllBytes($path, [byte[]]@())
}
$validOutput = & $check 2>&1
if ($LASTEXITCODE -ne 0 -or $validOutput) { throw 'Present-game-data preflight failed or wrote to output.' }
Write-Output "PASS: $($entries.Count) reviewed DLL mappings and five static-audio notices; missing data explained; data paths accepted without stdout."
