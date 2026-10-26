function Get-ReviewedFallbackAssets {
    param(
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)][string]$AllowlistPath
    )
    $root = (Resolve-Path -LiteralPath $SourceRoot).Path.TrimEnd('\')
    $manifestPath = Join-Path $root 'SOURCE_MANIFEST.json'
    $pin = '8AAA70E6F47B1F8ACD9060B2C3CA02C43A8B3D16009EBBF820C7F8AD5849B251'
    if ((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Fallback source root cannot be a reparse point.'
    }
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $pin) {
        throw 'Fallback source manifest differs from reviewed pin.'
    }
    $sourceManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $allowlist = Get-Content -LiteralPath $AllowlistPath -Raw | ConvertFrom-Json
    if ($sourceManifest.schema_version -ne 1 -or @($sourceManifest.entries).Count -ne 34 -or
        $allowlist.schema_version -ne 1 -or $allowlist.source_manifest_sha256 -ne $pin -or
        @($allowlist.runtime_files).Count -ne 30) {
        throw 'Fallback manifest or runtime allowlist shape differs from review.'
    }

    $sources = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $sourceManifest.entries) {
        $relative = [string]$entry.path
        if ($relative -notmatch '^[A-Za-z0-9_.+/-]+$' -or
            $relative -match '(^|/)\.\.?(?:/|$)' -or $sources.ContainsKey($relative)) {
            throw "Invalid or duplicate fallback source path: $relative"
        }
        $sources.Add($relative, $entry)
    }
    $actual = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
    $actualFiles = @($actual | Where-Object { -not $_.PSIsContainer })
    if ($actualFiles.Count -ne 35) { throw 'Unreviewed fallback source file count.' }
    foreach ($item in $actual) {
        $relative = $item.FullName.Substring($root.Length + 1).Replace('\','/')
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Fallback reparse point: $relative"
        }
        if ($item.PSIsContainer) {
            if ($relative -notin @('fonts','licenses','textures')) {
                throw "Unreviewed fallback directory: $relative"
            }
            continue
        }
        if ($relative -eq 'SOURCE_MANIFEST.json') { continue }
        if (-not $sources.ContainsKey($relative)) { throw "Unreviewed fallback file: $relative" }
        $record = $sources[$relative]
        if ($item.Length -ne $record.bytes -or
            (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash -ne $record.sha256) {
            throw "Fallback source file differs from manifest: $relative"
        }
    }

    $destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $runtimeSources = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($asset in $allowlist.runtime_files) {
        $from = [string]$asset.source_path
        $to = [string]$asset.destination_path
        $expectedDestination = if ($from -eq 'licenses/NotoSansCJK-OFL-1.1.txt') {
            'THIRD_PARTY_LICENSES/NotoSansCJK/OFL.txt'
        } elseif ($from -match '^(textures|fonts)/[A-Za-z0-9_.+-]+$') { $from }
        else { throw "Unreviewed runtime fallback source: $from" }
        $expectedLicense = if ($from -match '^fonts/' -or $from -match '^licenses/') { 'OFL-1.1' }
            else { 'GPL-2.0' }
        if ($to -cne $expectedDestination -or $asset.license -ne $expectedLicense -or
            -not $sources.ContainsKey($from) -or -not $destinations.Add($to) -or
            -not $runtimeSources.Add($from) -or $sources[$from].sha256 -ne $asset.sha256 -or
            $sources[$from].bytes -ne $asset.size -or $sources[$from].license -ne $asset.license) {
            throw "Fallback runtime mapping differs from reviewed source: $from"
        }
    }
    if (@($runtimeSources | Where-Object { $_ -match '^textures/' }).Count -ne 27 -or
        @($runtimeSources | Where-Object { $_ -match '^fonts/' }).Count -ne 2 -or
        -not $runtimeSources.Contains('licenses/NotoSansCJK-OFL-1.1.txt')) {
        throw 'Fallback runtime coverage is incomplete.'
    }
    return $allowlist
}
