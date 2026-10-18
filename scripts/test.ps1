param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$buildPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
Push-Location $repoRoot
try {
    & $buildPath Tests\CoreTests.vcxproj /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0
    if ($LASTEXITCODE -ne 0) { throw 'Core test build failed.' }
    & ".\artifacts\tests\$Configuration\CoreTests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    python Tests\smoke_capture.py $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'Capture tests failed.' }
} finally { Pop-Location }
