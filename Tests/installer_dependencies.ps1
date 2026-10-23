$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\Installer.Dependencies.ps1')
$sandbox = Join-Path ([IO.Path]::GetTempPath()) ('Aelura-dependency-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Write-Fixture([string]$Name, [string]$Xml) {
    $path = Join-Path $sandbox $Name
    $archive = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $writer = New-Object IO.StreamWriter($archive.CreateEntry('AppxManifest.xml').Open())
        try { $writer.Write($Xml) } finally { $writer.Dispose() }
    } finally { $archive.Dispose() }
    return $path
}
function New-Installed([string]$Name, [string]$Version) {
    [pscustomobject]@{ Name=$Name; Version=$Version; Publisher='CN=Fixture'; Architecture='X64'; IsFramework=$true; IsDevelopmentMode=$false; Status='Ok' }
}
# These mocks never invoke Windows deployment or touch any installed packages.
function Get-AppxPackage { param([string]$Name) $script:registered | Where-Object Name -eq $Name }
function Add-AppxPackage {
    [CmdletBinding()]
    param([string]$Path, [string[]]$DependencyPath, [switch]$DeferRegistrationWhenPackagesAreInUse)
    $script:deploymentParameters = $PSBoundParameters
    if ($script:deploymentError) { throw $script:deploymentError }
}
try {
    $package = Write-Fixture 'app.msix' '<Package><Identity Name="App" Publisher="CN=Fixture" Version="1.0.0.0" ProcessorArchitecture="x64"/><Dependencies><PackageDependency Name="Runtime" Publisher="CN=Fixture" MinVersion="2.0.0.0"/><PackageDependency Name="VCLibs" Publisher="CN=Fixture" MinVersion="14.0.0.0"/></Dependencies></Package>'
    $runtime = Write-Fixture 'runtime.msix' '<Package><Identity Name="Runtime" Publisher="CN=Fixture" Version="2.5.0.0" ProcessorArchitecture="x64"/><Properties><Framework>true</Framework></Properties></Package>'
    $vclibs = Write-Fixture 'vclibs.appx' '<Package><Identity Name="VCLibs" Publisher="CN=Fixture" Version="14.2.0.0" ProcessorArchitecture="x64"/><Properties><Framework>true</Framework></Properties></Package>'
    $paths = @($runtime, $vclibs)
    $installed = @((New-Installed 'Runtime' '2.0.0.0'), (New-Installed 'VCLibs' '15.0.0.0'))
    $plan = Get-InstallerDependencyPlan $package $paths $installed
    Assert ($plan.Paths.Count -eq 0) 'Satisfying minimum version must skip newer bundled dependencies'
    $app = New-Installed 'App' '1.0.0.0'
    $app.IsFramework = $false
    $script:registered = $installed + $app
    $result = Invoke-InstallerDeployment $plan
    Assert ($result.Complete -and $result.InstalledPackage.Name -eq 'App') 'Registered app should complete'
    Assert (!$script:deploymentParameters.ContainsKey('DependencyPath')) 'No empty dependency list or unnecessary updates'
    Assert $script:deploymentParameters.DeferRegistrationWhenPackagesAreInUse 'Must allow Windows to defer registration'
    $installed[0].Version = '1.0.0.0'
    $plan = Get-InstallerDependencyPlan $package $paths $installed
    Assert ($plan.Paths.Count -eq 1 -and $plan.Paths[0] -eq $runtime) 'Only outdated dependency should be submitted'
    $result = Invoke-InstallerDeployment $plan
    Assert (!$result.Complete) 'Pending dependency must prevent success even if app version is registered'
    Assert ($script:deploymentParameters.DependencyPath.Count -eq 1) 'Deployment should receive only missing dependencies'
    $installed[0].Version = '2.0.0.0'
    $plan = Get-InstallerDependencyPlan $package $paths $installed
    $script:registered = $installed
    Assert (!(Invoke-InstallerDeployment $plan).Complete) 'Unregistered deferred app must not report success'
    $app.Version = '0.9.0.0'
    $script:registered = $installed + $app
    Assert (!(Invoke-InstallerDeployment $plan).Complete) 'Old app registration must not report success'
    $app.Version = '1.0.0.0'
    $app.IsDevelopmentMode = $true
    Assert (!(Invoke-InstallerDeployment $plan).Complete) 'Existing development layout is not a completed signed installation'
    foreach ($property in @('Architecture', 'Publisher', 'IsFramework', 'Status')) {
        $prior = $installed[0].$property
        $installed[0].$property = switch ($property) { 'Architecture' { 'X86' }; 'Publisher' { 'CN=Other' }; 'IsFramework' { $false }; 'Status' { 'Modified' } }
        Assert ((Get-InstallerDependencyPlan $package $paths $installed).Paths.Count -eq 1) "Must not accept wrong $property"
        $installed[0].$property = $prior
    }
    Assert ((Get-InstallerDependencyPlan $package $paths @()).Paths.Count -eq 2) 'Clean device needs both dependencies'
    $rejected = $false
    try { Get-InstallerDependencyPlan $package @($vclibs) $installed | Out-Null } catch { $rejected = $_.Exception.Message -eq 'install.dependencies_missing' }
    Assert $rejected 'Incomplete archive must fail even on a machine with installed dependencies'
    $script:deploymentError = 'HRESULT 0x80073D02 fixture'
    $rejected = $false
    try { Invoke-InstallerDeployment $plan | Out-Null } catch { $rejected = $_.Exception.Message -match '80073D02' }
    Assert $rejected 'Deployment errors must propagate to the installer'
    Write-Output 'PASS: dependency identity/architecture/minimum/status selection, selective deployment, deferred registration and error propagation (no system changes)'
} finally {
    $resolved = [IO.Path]::GetFullPath($sandbox)
    if ($resolved.StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'Aelura-dependency-tests-*') { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
