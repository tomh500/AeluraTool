param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild not found. Install the Visual Studio C++/WinUI workload.' }
Push-Location $repoRoot
try {
    & $msbuild AeluraTool.slnx /m /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64 /p:VisualStudioVersion=18.0 /fl "/flp:logfile=artifacts\build-$($Configuration.ToLower()).log;encoding=UTF-8"
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
