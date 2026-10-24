param([switch]$Test,[switch]$CompileOnly)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
# Several agents share this checkout and this build/ directory. Serialize the build instead of
# letting two zig processes overwrite the same object files and executables. A file held with share
# mode None works across shells and Windows sessions, which a named mutex does not.
New-Item -ItemType Directory -Force build | Out-Null
$buildLock = $null
$lockDeadline = (Get-Date).AddMinutes(30)
while ($true) {
    try {
        $buildLock = [IO.File]::Open((Join-Path $PSScriptRoot 'build\.build-lock'),
                                     [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                                     [IO.FileShare]::None)
        break
    } catch [System.IO.IOException] {
        if ((Get-Date) -gt $lockDeadline) { throw 'Timed out waiting for the shared build lock.' }
        Start-Sleep -Milliseconds 400
    }
}
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot 'tools\cache'
    $zig = Join-Path $PSScriptRoot 'tools\zig-windows-x86_64-0.14.1\zig.exe'
    if (!(Test-Path -LiteralPath $zig)) { throw 'Run python scripts/bootstrap.py first.' }
    & $zig cc -O2 -c vendor/monocypher.c -o build/monocypher.o
    if ($LASTEXITCODE -ne 0) { throw 'Crypto dependency build failed.' }
    & $zig cc -O2 -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -c vendor/miniz_tinfl.c -o build/miniz_tinfl.o
    if ($LASTEXITCODE -ne 0) { throw 'Inflater build failed.' }
    $sources = @('src/core.cpp','src/platform.cpp','src/api.cpp','src/audio.cpp','src/session.cpp','src/bridge.cpp','src/agent.cpp','src/context.cpp','src/stats.cpp','src/persona.cpp','src/recommend.cpp','src/settings.cpp','src/ui.cpp','src/terminal.cpp','src/commands.cpp','src/mascot.cpp','src/service.cpp','src/mcp.cpp','vendor/qrcodegen.cpp','build/monocypher.o','build/miniz_tinfl.o')
    if ($Test) { $sources += 'tests/tests.cpp'; $target = 'build/mora-tests.exe' }
    else { $sources += 'src/main.cpp'; $target = 'build/mora.exe' }
    # Link to a fresh name: Windows refuses to overwrite an executable another teammate's TUI test
    # is still running, but it does allow renaming that file aside, so a running test never blocks
    # the build.
    $linkTarget = $target + '.new'
    & $zig c++ -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -Isrc -Ivendor -Iassets @sources -o $linkTarget -lwinhttp -lbcrypt -lcrypt32 -lole32 -luuid -lwindowscodecs -lmfplat -lmfreadwrite -lmfuuid -lwinmm -lpropsys -lpsapi -lshell32 -lshlwapi -ladvapi32 -static
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if (Test-Path -LiteralPath $target) {
        try {
            Move-Item -LiteralPath $linkTarget -Destination $target -Force -ErrorAction Stop
        } catch {
            $aside = $target + '.inuse-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
            Move-Item -LiteralPath $target -Destination $aside -Force
            Move-Item -LiteralPath $linkTarget -Destination $target -Force
        }
    } else {
        Move-Item -LiteralPath $linkTarget -Destination $target -Force
    }
    # Reclaim superseded images once the process that used them has exited.
    Get-ChildItem -LiteralPath build -Filter '*.inuse-*' -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { }
    }
    if ($Test -and !$CompileOnly) { & '.\build\mora-tests.exe'; if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' } }
} finally {
    Pop-Location
    if ($buildLock) {
        $buildLock.Dispose()
    }
}
