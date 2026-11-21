$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'Instale CMake 3.22+ e Visual Studio 2022 (Desenvolvimento para desktop com C++).'
}
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Falha na configuracao CMake.' }
cmake --build build --config Release --parallel --target SilverConsole SilverConsoleTests
if ($LASTEXITCODE -ne 0) { throw 'Falha na compilacao.' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Falha nos testes.' }
Write-Host 'Pronto: build/SilverConsole_artefacts/Release/SilverConsole.exe'
