param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('all', 'hash', 'migration')][string]$Suite = 'all',
    [switch]$CoreOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) not found.' }
$buildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$buildPath) { throw 'MSBuild not found. Install the Visual Studio C++ workload.' }
if (!(Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python 3 is required for resource and capture checks.' }
Push-Location $repoRoot
try {
    python Tests\check_i18n.py
    if ($LASTEXITCODE -ne 0) { throw 'Translation resource checks failed.' }
    & $buildPath Tests\CoreTests.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
    if ($LASTEXITCODE -ne 0) { throw 'Core test build failed.' }
    & ".\artifacts\tests\$Configuration\CoreTests.exe" --suite $Suite
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    if (!$CoreOnly) {
        & $buildPath Tests\AccountTests.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
        if ($LASTEXITCODE -ne 0) { throw 'Account test build failed.' }
        & ".\artifacts\tests\$Configuration\AccountTests.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Account tests failed.' }
        & $buildPath Tests\RecordingTests.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
        if ($LASTEXITCODE -ne 0) { throw 'Recording test build failed.' }
        & ".\artifacts\tests\$Configuration\RecordingTests.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Recording tests failed.' }
        & $buildPath Tests\CaptureSharingTests.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
        if ($LASTEXITCODE -ne 0) { throw 'Image sharing test build failed.' }
        & ".\artifacts\tests\$Configuration\CaptureSharingTests.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Image sharing tests failed.' }
        & $buildPath Tools\ScreenShotPlus\AeluraCapture.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
        if ($LASTEXITCODE -ne 0) { throw 'Capture component build failed.' }
        python Tests\smoke_capture.py $Configuration
        if ($LASTEXITCODE -ne 0) { throw 'Capture tests failed.' }
    }
    Write-Host "PASS: $Configuration tests completed (suite=$Suite, coreOnly=$CoreOnly)"
} finally { Pop-Location }
