$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$fixtureRoot = Join-Path $buildRoot ('packaging-fixture-' + [Guid]::NewGuid().ToString('N'))
$oldLocalAppData = $env:LOCALAPPDATA
$oldPath = $env:PATH
$checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}

function Write-FixtureFile([string]$Path, [string]$Content = 'fixture') {
    New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Content, (New-Object Text.UTF8Encoding($false)))
}

function New-InstallFixture([string]$Name) {
    $root = Join-Path $fixtureRoot $Name
    $source = Join-Path $root 'source'
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    foreach ($file in @('install.ps1', 'install.cmd', 'mora.cmd')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination (Join-Path $source $file)
    }
    Write-FixtureFile (Join-Path $source 'mora.exe') 'fixture executable'
    Write-FixtureFile (Join-Path $source 'docs\nested\plan.md') 'first revision'
    Write-FixtureFile (Join-Path $source 'docs\home-preview.png') 'preview'
    Write-FixtureFile (Join-Path $source 'licenses\dependency.LICENSE') 'dependency license'
    Write-FixtureFile (Join-Path $source 'assets\amu\portrait.png') 'portrait'
    $local = Join-Path $root 'local'
    $commands = Join-Path $root 'commands'
    New-Item -ItemType Directory -Path $local, $commands -Force | Out-Null
    return @{ root=$root; source=$source; local=$local; commands=$commands; install=(Join-Path $local 'Programs\MORA') }
}

function Assert-InstallerRejected($Fixture, [string]$CommandDir) {
    $env:LOCALAPPDATA = $Fixture.local
    $rejected = $false
    try { & (Join-Path $Fixture.source 'install.ps1') -CommandDir $CommandDir *> $null }
    catch { $rejected = $true }
    Assert-True $rejected 'Installer should reject the unsafe target before modifying it.'
}

try {
    $normal = New-InstallFixture 'normal'
    $env:LOCALAPPDATA = $normal.local
    & (Join-Path $normal.source 'install.ps1') -CommandDir $normal.commands *> $null
    Assert-True (Test-Path -LiteralPath (Join-Path $normal.install 'mora.exe')) 'EXE missing.'
    Assert-True (Test-Path -LiteralPath (Join-Path $normal.install 'docs\home-preview.png')) 'Product preview missing.'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $normal.install 'docs\nested\plan.md'))) 'Private development docs entered installation.'
    Assert-True (Test-Path -LiteralPath (Join-Path $normal.install 'licenses\dependency.LICENSE')) 'Licenses missing.'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $normal.install 'assets\amu\portrait.png'))) 'Source assets entered installation.'
    $shim = Get-Content -LiteralPath (Join-Path $normal.commands 'mora.cmd') -Raw
    Assert-True ($shim.Contains('DisableDelayedExpansion')) 'Shim must protect paths from delayed expansion.'
    Assert-True ($shim.Contains('%LOCALAPPDATA%\Programs\MORA\mora.exe')) 'Shim must use the current user installation.'
    Write-FixtureFile (Join-Path $normal.source 'docs\nested\plan.md') 'second revision'
    & (Join-Path $normal.source 'install.ps1') -CommandDir $normal.commands *> $null
    Assert-True (!(Test-Path -LiteralPath (Join-Path $normal.install 'docs\nested\plan.md'))) 'Reinstall copied private docs.'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $normal.install 'docs\docs'))) 'Reinstall nested docs/docs.'
    $record = Get-Content -LiteralPath (Join-Path $normal.install 'install.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-True ($record.applicationId -eq 'mora-player') 'Installation ownership marker missing.'
    $mainExe = [IO.Path]::GetFullPath((Join-Path $projectRoot '..\..\build\mora.exe'))
    if (Test-Path -LiteralPath $mainExe -PathType Leaf) {
        Copy-Item -LiteralPath $mainExe -Destination (Join-Path $normal.source 'mora.exe') -Force
        & (Join-Path $normal.source 'install.ps1') -CommandDir $normal.commands *> $null
        $env:PATH = $normal.commands + ';' + $oldPath
        Push-Location -LiteralPath $normal.root
        try {
            $version = & $env:ComSpec /d /c 'mora --version'
            Assert-True ($LASTEXITCODE -eq 0 -and ($version -join '') -match '^MORA ') 'Installed CMD command did not launch the actual native executable.'
        } finally { Pop-Location }
        $env:PATH = $oldPath
    }

    $commandConflict = New-InstallFixture 'command-conflict'
    Write-FixtureFile (Join-Path $commandConflict.commands 'mora.cmd') 'unrelated launcher'
    Assert-InstallerRejected $commandConflict $commandConflict.commands
    Assert-True (!(Test-Path -LiteralPath $commandConflict.install)) 'Command conflict modified the install root.'
    Assert-True ((Get-Content -LiteralPath (Join-Path $commandConflict.commands 'mora.cmd') -Raw) -eq 'unrelated launcher') 'Foreign command was overwritten.'

    $programConflict = New-InstallFixture 'program-conflict'
    Write-FixtureFile (Join-Path $programConflict.install 'mora.exe') 'unrelated program'
    Assert-InstallerRejected $programConflict $programConflict.commands
    Assert-True ((Get-Content -LiteralPath (Join-Path $programConflict.install 'mora.exe') -Raw) -eq 'unrelated program') 'Foreign program was overwritten.'

    $invalidCommandDir = New-InstallFixture 'invalid-command-dir'
    Assert-InstallerRejected $invalidCommandDir (Join-Path $invalidCommandDir.root 'absent')
    Assert-True (!(Test-Path -LiteralPath $invalidCommandDir.install)) 'Invalid CommandDir modified the install root.'
    Assert-InstallerRejected $invalidCommandDir 'C:relative-path'
    Assert-True (!(Test-Path -LiteralPath $invalidCommandDir.install)) 'Drive-relative CommandDir modified the install root.'

    $exeConflict = New-InstallFixture 'exe-conflict'
    Write-FixtureFile (Join-Path $exeConflict.commands 'mora.exe') 'another command'
    Assert-InstallerRejected $exeConflict $exeConflict.commands
    Assert-True (!(Test-Path -LiteralPath $exeConflict.install)) 'Existing command EXE was not preflighted.'

    $packageRoot = Join-Path $fixtureRoot 'package'
    New-Item -ItemType Directory -Path (Join-Path $packageRoot 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'scripts\package.ps1') -Destination (Join-Path $packageRoot 'scripts\package.ps1') -Force
    foreach ($file in @('mora.cmd','install.cmd','install.ps1','README.md','DEVELOPMENT_PLAN.md','HANDOFF.md','THIRD-PARTY-NOTICES.md','LICENSE','AGENTS.md','.clang-format','.gitignore','build.ps1')) {
        Write-FixtureFile (Join-Path $packageRoot $file)
    }
    foreach ($file in @('build\mora.exe','src\main.cpp','tests\unit.cpp','docs\upstream-LICENSE','docs\TOOL_CONTRACTS.md','docs\plan.md','docs\home-preview.png','assets\amu\portrait.png','assets\amu\amu-source.png','assets\amu\provenance.json','assets\amu\indexed.hpp','vendor\json.LICENSE','vendor\monocypher.LICENSE','vendor\miniz.LICENSE','vendor\qrcodegen.hpp')) {
        Write-FixtureFile (Join-Path $packageRoot $file)
    }
    foreach ($file in @('docs\session.bin','assets\amu\.env','tests\test-profile\session.bin','tools\private.txt','build\agent.bin')) {
        Write-FixtureFile (Join-Path $packageRoot $file) 'must not ship'
    }
    & (Join-Path $packageRoot 'scripts\package.ps1') *> $null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $binaryZip = Join-Path $packageRoot 'dist\MORA-0.7.2-preview-win-x64.zip'
    $sourceZip = Join-Path $packageRoot 'dist\MORA-0.7.2-preview-source.zip'
    foreach ($zipPath in @($binaryZip,$sourceZip)) {
        $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            $names = @($archive.Entries | ForEach-Object { $_.FullName })
            Assert-True ($names -notcontains 'assets/amu/amu-source.png') 'Original portrait entered a release archive.'
            if ($zipPath -eq $binaryZip) {
                Assert-True ($names -contains 'mora.exe') 'Windows release has no executable.'
                Assert-True ($names -contains 'mora.cmd') 'Windows release has no launcher.'
                Assert-True ($names -contains 'docs/home-preview.png') 'Product preview missing.'
                Assert-True ($names -notcontains 'DEVELOPMENT_PLAN.md' -and $names -notcontains 'HANDOFF.md') 'Development notes entered Windows release.'
                Assert-True (@($names | Where-Object { $_ -like 'assets/*' }).Count -eq 0) 'Compiled assets entered Windows release.'
            } else {
                Assert-True ($names -notcontains 'DEVELOPMENT_PLAN.md' -and $names -notcontains 'HANDOFF.md') 'Private development records entered source archive.'
                Assert-True ($names -contains 'assets/amu/indexed.hpp') 'Source archive lost indexed sprite.'
            }
            $license = if ($zipPath -eq $binaryZip) { 'licenses/json.LICENSE' } else { 'vendor/json.LICENSE' }
            Assert-True ($names -contains $license) 'License text not packaged.'
            Assert-True (!(($names -join '|') -match '(session\.bin|agent\.bin|\.env|test-profile|tools/)')) 'A credential/profile file entered the archive.'
        } finally { $archive.Dispose() }
    }
    $firstHash = (Get-FileHash -LiteralPath $binaryZip -Algorithm SHA256).Hash
    $sourceHash = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash
    & (Join-Path $packageRoot 'scripts\package.ps1') *> $null
    Assert-True ((Get-FileHash -LiteralPath $binaryZip -Algorithm SHA256).Hash -eq $firstHash) 'Identical input produced a different binary archive.'
    Assert-True ((Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash -eq $sourceHash) 'Identical input produced a different source archive.'
    $release = Join-Path $packageRoot 'dist\MORA-0.7.2-preview-win-x64'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $release 'docs\docs'))) 'Repeated packaging nested docs.'
    Write-FixtureFile (Join-Path $release 'keep.txt') 'unrelated addition'
    $refused = $false
    try { & (Join-Path $packageRoot 'scripts\package.ps1') *> $null } catch { $refused = $true }
    Assert-True $refused 'Packaging should preserve unowned additions in its output directory.'
    Assert-True (Test-Path -LiteralPath (Join-Path $release 'keep.txt')) 'Packaging deleted an unowned file.'
    Write-Host "PASS $checks packaging and installer checks"
} finally {
    $env:LOCALAPPDATA = $oldLocalAppData
    $env:PATH = $oldPath
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $resolvedBuild = [IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
    if ($resolvedFixture.StartsWith($resolvedBuild, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolvedFixture -Leaf).StartsWith('packaging-fixture-')) {
        if (Test-Path -LiteralPath $resolvedFixture) { Remove-Item -LiteralPath $resolvedFixture -Recurse -Force }
    } else { throw 'Refusing cleanup outside the fixture directory.' }
}
