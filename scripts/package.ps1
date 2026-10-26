$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$dist = [IO.Path]::GetFullPath((Join-Path $projectRoot 'dist'))
$release = [IO.Path]::GetFullPath((Join-Path $dist 'MORA-0.7.2-preview-win-x64'))
$utf8 = New-Object Text.UTF8Encoding($false)

function Assert-Within([string]$Path, [string]$Root) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $boundary = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package target escaped its workspace: $resolved"
    }
}

function Assert-NoLink([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Linked package paths are not supported: $Path"
        }
    }
}

function Test-AllowedEntry([string]$RelativePath) {
    $path = $RelativePath.Replace('\','/').ToLowerInvariant()
    if ($path -match '^assets/.+-source\.(png|jpe?g|webp)$') { return $false }
    if ($path -match '(^|/)(tools|build|dist|\.git|\.worktrees|__pycache__|node_modules|\.venv|test-profile|test-profiles|profile|profiles)(/|$)') { return $false }
    $name = ($path -split '/')[-1]
    if ($name -match '^(session\.(bin|tmp)|agent\.bin|install\.json|\.env(\..*)?|credentials?\.json|secrets?\.json)$') { return $false }
    if ($name -match '\.(key|pem|pfx|p12)$') { return $false }
    return $true
}

function Add-File($Entries, [string]$Source, [string]$RelativePath) {
    if (!(Test-AllowedEntry $RelativePath)) { return }
    if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required package file missing: $Source" }
    Assert-NoLink $Source
    $Entries.Add([pscustomobject]@{ source=$Source; relative=$RelativePath.Replace('\','/') })
}

function Add-Tree($Entries, [string]$Source, [string]$RelativePath) {
    if (!(Test-AllowedEntry $RelativePath)) { return }
    if (!(Test-Path -LiteralPath $Source -PathType Container)) { return }
    Assert-NoLink $Source
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force | Sort-Object Name) {
        $relative = $RelativePath + '/' + $item.Name
        if (!(Test-AllowedEntry $relative)) { continue }
        Assert-NoLink $item.FullName
        if ($item.PSIsContainer) { Add-Tree $Entries $item.FullName $relative }
        else { Add-File $Entries $item.FullName $relative }
    }
}

function Write-Archive([string]$Target, $Entries) {
    Assert-Within $Target $dist
    $temporary = Join-Path $dist ('.archive-' + [Guid]::NewGuid().ToString('N') + '.zip')
    Assert-Within $temporary $dist
    $archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create, [Text.Encoding]::UTF8)
    try {
        foreach ($item in $Entries | Sort-Object relative) {
            $entry = $archive.CreateEntry($item.relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::Parse('2000-01-01T00:00:00+00:00')
            $inputStream = [IO.File]::OpenRead($item.source)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $archive.Dispose() }
    try { Copy-Item -LiteralPath $temporary -Destination $Target -Force }
    finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
}

Assert-Within $dist $projectRoot
Assert-Within $release $dist
Assert-NoLink $dist
Assert-NoLink $release
$binary = New-Object 'Collections.Generic.List[object]'
# The EXE is the only file admitted from build/.
$sourceExe = Join-Path $projectRoot 'build\mora.exe'
if (!(Test-Path -LiteralPath $sourceExe -PathType Leaf)) { throw 'Build mora.exe before packaging.' }
$binary.Add([pscustomobject]@{ source=$sourceExe; relative='mora.exe' })
$runtimeFiles = @('mora.cmd','install.cmd','install.ps1','README.md','THIRD-PARTY-NOTICES.md','LICENSE')
foreach ($name in $runtimeFiles) { Add-File $binary (Join-Path $projectRoot $name) $name }
Add-File $binary (Join-Path $projectRoot 'docs\home-preview.png') 'docs/home-preview.png'
foreach ($name in @('json.LICENSE','monocypher.LICENSE','miniz.LICENSE')) {
    Add-File $binary (Join-Path $projectRoot ('vendor\' + $name)) ('licenses/' + $name)
}
Add-File $binary (Join-Path $projectRoot 'docs\upstream-LICENSE') 'licenses/api-enhanced.LICENSE'
Add-File $binary (Join-Path $projectRoot 'vendor\qrcodegen.hpp') 'licenses/qrcodegen-with-license.hpp'

# Refuse unrelated or modified files before clearing an earlier generated bundle.
$manifestPath = Join-Path $release '.mora-package.json'
if (Test-Path -LiteralPath $release) {
    $existing = @(Get-ChildItem -LiteralPath $release -Force)
    if ($existing.Count -gt 0) {
        if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Existing release directory is not owned by the package script.' }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($manifest.packageId -ne 'mora-framework-win-x64') { throw 'Package ownership marker does not match.' }
        $owned = @{}
        foreach ($file in $manifest.files) { $owned[$file.relativePath] = $file.sha256 }
        foreach ($item in Get-ChildItem -LiteralPath $release -Recurse -Force) {
            Assert-NoLink $item.FullName
            if ($item.PSIsContainer) { continue }
            $relative = $item.FullName.Substring($release.Length + 1).Replace('\','/')
            if ($relative -eq '.mora-package.json') { continue }
            if (!$owned.ContainsKey($relative)) { throw "Unowned file in the release directory; preserving it: $relative" }
            if ((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash -ne $owned[$relative]) { throw "Modified release file; preserving it: $relative" }
        }
    }
    # Both resolved targets are verified under this project's dist/, and every existing
    # leaf above was checked against the script's own hash manifest before recursive removal.
    Assert-Within $release $dist
    Assert-Within $dist $projectRoot
    Remove-Item -LiteralPath $release -Recurse -Force
}
New-Item -ItemType Directory -Path $release -Force | Out-Null
$manifestFiles = @()
foreach ($file in $binary | Sort-Object relative) {
    $target = Join-Path $release $file.relative.Replace('/','\')
    Assert-Within $target $release
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $file.source -Destination $target -Force
    $manifestFiles += [ordered]@{ relativePath=$file.relative; sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash }
}
$manifest = [ordered]@{ packageId='mora-framework-win-x64'; version='0.7.2-preview'; files=$manifestFiles }
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5), $utf8)
$releaseEntries = New-Object 'Collections.Generic.List[object]'
foreach ($file in $manifestFiles) { $releaseEntries.Add([pscustomobject]@{ source=(Join-Path $release $file.relativePath.Replace('/','\')); relative=$file.relativePath }) }
$releaseEntries.Add([pscustomobject]@{ source=$manifestPath; relative='.mora-package.json' })

$source = New-Object 'Collections.Generic.List[object]'
foreach ($directory in @('src','tests','scripts','vendor','assets')) {
    Add-Tree $source (Join-Path $projectRoot $directory) $directory
}
foreach ($name in @('home-preview.png','TOOL_CONTRACTS.md','upstream-LICENSE')) {
    Add-File $source (Join-Path $projectRoot ('docs\' + $name)) ('docs/' + $name)
}
$sourceFiles = @('README.md','THIRD-PARTY-NOTICES.md','LICENSE','.clang-format','.gitignore','build.ps1','mora.cmd','install.cmd','install.ps1')
foreach ($name in $sourceFiles) { Add-File $source (Join-Path $projectRoot $name) $name }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$binaryZip = Join-Path $dist 'MORA-0.7.2-preview-win-x64.zip'
$sourceZip = Join-Path $dist 'MORA-0.7.2-preview-source.zip'
Write-Archive $binaryZip $releaseEntries
Write-Archive $sourceZip $source
Get-FileHash -Algorithm SHA256 -LiteralPath $sourceExe | Format-List
Get-Item -LiteralPath $binaryZip,$sourceZip | Select-Object FullName,Length
