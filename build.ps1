param([switch]$Test,[switch]$CompileOnly)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot 'tools\cache'
    $zig = Join-Path $PSScriptRoot 'tools\zig-windows-x86_64-0.14.1\zig.exe'
    if (!(Test-Path -LiteralPath $zig)) { throw 'Run python scripts/bootstrap.py first.' }
    & $zig cc -O2 -c vendor/monocypher.c -o build/monocypher.o
    if ($LASTEXITCODE -ne 0) { throw 'Crypto dependency build failed.' }
    & $zig cc -O2 -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -c vendor/miniz_tinfl.c -o build/miniz_tinfl.o
    if ($LASTEXITCODE -ne 0) { throw 'Inflater build failed.' }
    $sources = @('src/core.cpp','src/platform.cpp','src/api.cpp','src/audio.cpp','src/session.cpp','src/bridge.cpp','src/agent.cpp','src/persona.cpp','src/recommend.cpp','src/settings.cpp','src/ui.cpp','src/terminal.cpp','src/commands.cpp','src/mascot.cpp','src/service.cpp','src/mcp.cpp','vendor/qrcodegen.cpp','build/monocypher.o','build/miniz_tinfl.o')
    if ($Test) { $sources += 'tests/tests.cpp'; $target = 'build/mora-tests.exe' }
    else { $sources += 'src/main.cpp'; $target = 'build/mora.exe' }
    & $zig c++ -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -Isrc -Ivendor -Iassets @sources -o $target -lwinhttp -lbcrypt -lcrypt32 -lole32 -luuid -lwindowscodecs -lmfplat -lmfreadwrite -lmfuuid -lwinmm -lpropsys -lpsapi -lshell32 -lshlwapi -ladvapi32 -static
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if ($Test -and !$CompileOnly) { & '.\build\mora-tests.exe'; if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' } }
} finally { Pop-Location }
