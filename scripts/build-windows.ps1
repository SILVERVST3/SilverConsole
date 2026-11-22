# Compila o SilverConsole no Windows, roda os testes e monta a pasta pronta para uso.
#
#   powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
#   ... -BuildDir C:\scb\build    pasta de build curta (evita o limite de 260 caracteres do MSVC)
#   ... -JuceDir C:\scb\juce      JUCE 8.0.6 ja baixado (sem download pelo CMake)
#   ... -SkipTests                so compila e empacota
param(
    [string]$BuildDir = '',
    [string]$JuceDir = '',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if ($BuildDir -eq '') { $BuildDir = Join-Path $root 'build' }

# Prefere o CMake que vem com o Visual Studio: outros CMake no PATH (ex.: MinGW)
# podem falhar no download do JUCE por certificado SSL e nao conhecem o MSVC.
$cmake = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) {
        $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) {
    $found = Get-Command cmake -ErrorAction SilentlyContinue
    if ($found) { $cmake = $found.Source }
}
if (-not $cmake) {
    throw 'Instale o Visual Studio 2022 (ou Build Tools) com "Desenvolvimento para desktop com C++".'
}
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
Write-Host "CMake: $cmake"

if (($BuildDir.Length + 140) -gt 260) {
    Write-Warning "Caminho de build longo ($BuildDir). Se aparecer 'No such file or directory' em arquivos do JUCE, use -BuildDir com um caminho curto."
}

$configureArgs = @('-S', '.', '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($JuceDir -ne '') { $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$JuceDir" }
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'Falha na configuracao CMake.' }

$targets = @('SilverConsole', 'ScPluginScanWorker', 'ScDspWorker')
if (-not $SkipTests) { $targets += 'SilverConsoleTests' }
& $cmake --build $BuildDir --config Release --parallel --target @targets
if ($LASTEXITCODE -ne 0) { throw 'Falha na compilacao.' }

if (-not $SkipTests) {
    & $ctest --test-dir $BuildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Falha nos testes.' }
}

# Pasta pronta para uso (mesmo conteudo do pacote do CI)
$out = Join-Path $root 'dist\SilverConsole'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Copy-Item (Join-Path $BuildDir 'SilverConsole_artefacts\Release\SilverConsole.exe') $out -Force
Copy-Item (Join-Path $BuildDir 'ScPluginScanWorker_artefacts\Release\ScPluginScanWorker.exe') $out -Force
Copy-Item (Join-Path $BuildDir 'ScDspWorker_artefacts\Release\ScDspWorker.exe') $out -Force
Copy-Item README.md, LICENSE, THIRD_PARTY.md $out -Force
$juceLicense = if ($JuceDir -ne '') { Join-Path $JuceDir 'LICENSE.md' } else { Join-Path $BuildDir '_deps\juce-src\LICENSE.md' }
if (Test-Path $juceLicense) { Copy-Item $juceLicense (Join-Path $out 'JUCE-LICENSE.md') -Force }
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python -and $python.Source -notlike '*WindowsApps*') {
    & $python.Source scripts/create-demo.py (Join-Path $out 'Demo')
}
Write-Host "Pronto: $out\SilverConsole.exe"
