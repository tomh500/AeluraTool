param([string]$CertificateThumbprint)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) not found.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild not found. Install the Visual Studio C++/WinUI workload.' }
$packageRoot = Join-Path $repoRoot 'artifacts\packages\'
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
$signing = @('/p:AppxPackageSigningEnabled=false')
if ($CertificateThumbprint) {
    if ($CertificateThumbprint -notmatch '^[0-9a-fA-F]{40}$') { throw 'Expected a 40-character SHA-1 certificate thumbprint.' }
    $signing = @('/p:AppxPackageSigningEnabled=true', "/p:PackageCertificateThumbprint=$CertificateThumbprint")
}
Push-Location $repoRoot
try {
    & $msbuild AeluraTool.vcxproj /m /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:VisualStudioVersion=18.0 /p:GenerateAppxPackageOnBuild=true /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxBundle=Never "/p:AppxPackageDir=$packageRoot" @signing /fl '/flp:logfile=artifacts\package-release.log;encoding=UTF-8'
    if ($LASTEXITCODE -ne 0) { throw "Packaging failed with exit code $LASTEXITCODE" }
    if (!$env:CI) { & (Join-Path $PSScriptRoot 'deploy.ps1') -Configuration Release }
    Write-Output "Package output: $packageRoot"
    if (!$CertificateThumbprint) { Write-Warning 'Package is unsigned. Sign it with a trusted certificate before installation or distribution.' }
} finally { Pop-Location }
