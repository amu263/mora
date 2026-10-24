param([string]$CommandDir)
$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+)') {
        throw 'Installation paths must be absolute.'
    }
    $resolved = [IO.Path]::GetFullPath($Path)
    if ($resolved.Equals([IO.Path]::GetPathRoot($resolved), [StringComparison]::OrdinalIgnoreCase)) { return $resolved }
    return $resolved.TrimEnd('\')
}

function Assert-OrdinaryPath([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing a linked installation target: $Path"
        }
    }
}

function Copy-TreeContents([string]$Source, [string]$Destination) {
    Assert-OrdinaryPath $Source
    Assert-OrdinaryPath $Destination
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing a linked package entry: $($item.FullName)"
        }
        $target = Join-Path $Destination $item.Name
        Assert-OrdinaryPath $target
        if ($item.PSIsContainer) { Copy-TreeContents $item.FullName $target }
        else { Copy-Item -LiteralPath $item.FullName -Destination $target -Force }
    }
}

function Assert-OrdinaryTree([string]$Path) {
    Assert-OrdinaryPath $Path
    if (Test-Path -LiteralPath $Path -PathType Container) {
        foreach ($item in Get-ChildItem -LiteralPath $Path -Force) {
            Assert-OrdinaryPath $item.FullName
            if ($item.PSIsContainer) { Assert-OrdinaryTree $item.FullName }
        }
    }
}

$localRoot = Get-FullPath $env:LOCALAPPDATA
if (!(Test-Path -LiteralPath $localRoot -PathType Container)) {
    throw 'LOCALAPPDATA must identify an existing directory for the current user.'
}
if ($localRoot.Equals([IO.Path]::GetPathRoot($localRoot), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'LOCALAPPDATA cannot be a filesystem root.'
}
$programsRoot = Join-Path $localRoot 'Programs'
$installRoot = Get-FullPath (Join-Path $programsRoot 'MORA')
Assert-OrdinaryPath $programsRoot
Assert-OrdinaryPath $installRoot
$sourceExe = Join-Path $PSScriptRoot 'mora.exe'
if (!(Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    $sourceExe = Join-Path $PSScriptRoot 'build\mora.exe'
}
Assert-OrdinaryPath $sourceExe
if (!(Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw 'mora.exe not found. Build or extract the release package first.'
}

# Finish all ownership and command checks before copying files or changing PATH.
$installRecord = Join-Path $installRoot 'install.json'
if (Test-Path -LiteralPath $installRoot) {
    if (!(Test-Path -LiteralPath $installRoot -PathType Container)) {
        throw "Installation target is not a directory: $installRoot"
    }
    $entries = @(Get-ChildItem -LiteralPath $installRoot -Force)
    if ($entries.Count -gt 0) {
        if (!(Test-Path -LiteralPath $installRecord -PathType Leaf)) {
            throw "Existing installation directory is not owned by MORA: $installRoot"
        }
        try { $previous = Get-Content -LiteralPath $installRecord -Raw -Encoding UTF8 | ConvertFrom-Json }
        catch { throw 'The existing MORA installation record is invalid.' }
        if ($previous.applicationId -ne 'mora-player' -or
            !(Get-FullPath $previous.installRoot).Equals($installRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Existing installation directory is not owned by MORA: $installRoot"
        }
    }
}

Assert-OrdinaryTree $installRoot
if ($CommandDir) {
    $shimRoot = Get-FullPath $CommandDir
    if (!(Test-Path -LiteralPath $shimRoot -PathType Container)) {
        throw 'CommandDir must be an existing directory on PATH.'
    }
    if ($shimRoot.Equals([IO.Path]::GetPathRoot($shimRoot), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'CommandDir cannot be a filesystem root.'
    }
} else { $shimRoot = $installRoot }
Assert-OrdinaryPath $shimRoot
$shimFile = Join-Path $shimRoot 'mora.cmd'
$shim = '@echo off' + "`r`n" + 'setlocal DisableDelayedExpansion' + "`r`n" +
    'rem MORA command launcher' + "`r`n" +
    '"%LOCALAPPDATA%\Programs\MORA\mora.exe" %*' + "`r`n" + 'exit /b %errorlevel%' + "`r`n"
$legacyShim = '@echo off' + "`r`n" + 'rem MORA command launcher' + "`r`n" +
    '"%LOCALAPPDATA%\Programs\MORA\mora.exe" %*' + "`r`n"
if (Test-Path -LiteralPath $shimFile) {
    if (!(Test-Path -LiteralPath $shimFile -PathType Leaf)) { throw "Command target is not a file: $shimFile" }
    Assert-OrdinaryPath $shimFile
    $existing = Get-Content -LiteralPath $shimFile -Raw
    if ($existing -ne $shim -and $existing -ne $legacyShim) {
        throw "Existing command is not owned by MORA: $shimFile"
    }
}
foreach ($name in @('mora.exe','mora.com','mora.bat','mora.ps1')) {
    $conflict = Join-Path $shimRoot $name
    $isOwnExe = $name -eq 'mora.exe' -and $shimRoot.Equals($installRoot, [StringComparison]::OrdinalIgnoreCase)
    if (!$isOwnExe -and (Test-Path -LiteralPath $conflict)) {
        throw "Another command would conflict with MORA: $conflict"
    }
}
foreach ($name in @('licenses')) {
    $source = Join-Path $PSScriptRoot $name
    if (Test-Path -LiteralPath $source) {
        Assert-OrdinaryTree $source
    }
}

New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
$targetExe = Join-Path $installRoot 'mora.exe'
if (!(Get-FullPath $sourceExe).Equals($targetExe, [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $sourceExe -Destination $targetExe -Force
}
foreach ($name in @('README.md','THIRD-PARTY-NOTICES.md','LICENSE')) {
    $source = Join-Path $PSScriptRoot $name
    $target = Join-Path $installRoot $name
    Assert-OrdinaryPath $target
    if ((Test-Path -LiteralPath $source -PathType Leaf) -and
        !(Get-FullPath $source).Equals($target, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}
foreach ($name in @('licenses')) {
    $source = Join-Path $PSScriptRoot $name
    $target = Join-Path $installRoot $name
    if ((Test-Path -LiteralPath $source -PathType Container) -and
        !(Get-FullPath $source).Equals($target, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-TreeContents $source $target
    }
}
$preview = Join-Path $PSScriptRoot 'docs\home-preview.png'
if (Test-Path -LiteralPath $preview -PathType Leaf) {
    Assert-OrdinaryPath $preview
    $previewRoot = Join-Path $installRoot 'docs'
    Assert-OrdinaryPath $previewRoot
    New-Item -ItemType Directory -Path $previewRoot -Force | Out-Null
    $targetPreview = Join-Path $previewRoot 'home-preview.png'
    Assert-OrdinaryPath $targetPreview
    if (!(Get-FullPath $preview).Equals($targetPreview, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $preview -Destination $targetPreview -Force
    }
}
# A source checkout carries license texts in vendor/, while release packages use licenses/.
if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'licenses')) -and
    (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'vendor') -PathType Container)) {
    $licenseRoot = Join-Path $installRoot 'licenses'
    New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
    foreach ($name in @('json.LICENSE','monocypher.LICENSE','miniz.LICENSE','qrcodegen.hpp')) {
        $source = Join-Path $PSScriptRoot ('vendor\' + $name)
        if (Test-Path -LiteralPath $source -PathType Leaf) { Copy-Item -LiteralPath $source -Destination (Join-Path $licenseRoot $name) -Force }
    }
    $upstream = Join-Path $PSScriptRoot 'docs\upstream-LICENSE'
    if (Test-Path -LiteralPath $upstream -PathType Leaf) { Copy-Item -LiteralPath $upstream -Destination (Join-Path $licenseRoot 'api-enhanced.LICENSE') -Force }
}
[IO.File]::WriteAllText($shimFile, $shim, [Text.Encoding]::ASCII)
$record = [ordered]@{
    applicationId='mora-player'; installRoot=$installRoot; commandFile=$shimFile
    commandDirProvided=[bool]$CommandDir; version='0.6.0-preview'
}
[IO.File]::WriteAllText($installRecord, ($record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
if (!$CommandDir) {
    $currentPath = [Environment]::GetEnvironmentVariable('Path','User')
    $entries = @($currentPath -split ';' | Where-Object { $_ })
    $alreadyPresent = $false
    foreach ($entry in $entries) {
        try {
            if ((Get-FullPath ([Environment]::ExpandEnvironmentVariables($entry))).Equals($installRoot, [StringComparison]::OrdinalIgnoreCase)) { $alreadyPresent=$true }
        } catch { }
    }
    if (!$alreadyPresent) { [Environment]::SetEnvironmentVariable('Path', (($entries + $installRoot) -join ';'), 'User') }
}
Write-Host 'MORA installed. Type mora in CMD or PowerShell.'
if (!$CommandDir) { Write-Host 'Open a new terminal window to refresh PATH.' }
Write-Host "Program: $targetExe"
