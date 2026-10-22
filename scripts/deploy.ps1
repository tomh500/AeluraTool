param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$outputRoot = Join-Path $repoRoot "x64\$Configuration\AeluraTool"
$recipePath = Join-Path $outputRoot 'AeluraTool.build.appxrecipe'
if (!(Test-Path -LiteralPath $recipePath)) { throw 'Build this configuration first.' }
[xml]$recipe = Get-Content -LiteralPath $recipePath -Raw
[xml]$manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'Package.appxmanifest') -Raw
$running = Get-Process AeluraTool, StrikeSense -ErrorAction SilentlyContinue
if ($running) { throw 'Close AeluraTool and StrikeSense normally before deployment; active tasks will not be interrupted.' }
$layoutRoot = [IO.Path]::GetFullPath((Join-Path $outputRoot ("AppX-" + $manifest.Package.Identity.Version)))
foreach ($item in $recipe.Project.ItemGroup.ChildNodes) {
    if (!$item.PackagePath -or !$item.Include) { continue }
    $targetPath = [IO.Path]::GetFullPath((Join-Path $layoutRoot $item.PackagePath))
    if (!$targetPath.StartsWith($layoutRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package layout path.' }
    New-Item -ItemType Directory -Path (Split-Path $targetPath -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $item.Include -Destination $targetPath -Force
}
$previous = Get-AppxPackage -Name $manifest.Package.Identity.Name
$restoreManifest = $null
if ($previous -and $previous.InstallLocation -ne $layoutRoot -and $previous.IsDevelopmentMode) {
    $restoreManifest = Join-Path $previous.InstallLocation 'AppxManifest.xml'
    Remove-AppxPackage -Package $previous.PackageFullName -PreserveApplicationData
}
try { Add-AppxPackage -Register (Join-Path $layoutRoot 'AppxManifest.xml') }
catch {
    if ($restoreManifest -and (Test-Path -LiteralPath $restoreManifest)) { Add-AppxPackage -Register $restoreManifest }
    throw
}
$installed = Get-AppxPackage -Name $manifest.Package.Identity.Name
if (!$installed -or $installed.InstallLocation -ne $layoutRoot) { throw 'Registered package does not match the new layout.' }
if ((Get-FileHash (Join-Path $layoutRoot 'AeluraTool.exe')).Hash -ne (Get-FileHash (Join-Path $outputRoot 'AeluraTool.exe')).Hash) { throw 'Deployed executable does not match the build.' }
$desktop = [Environment]::GetFolderPath('Desktop')
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path $desktop (([char]0x9065).ToString() + [char]0x6C50 + [char]0x5DE5 + [char]0x5177 + [char]0x7BB1 + '.lnk')))
$shortcut.TargetPath = Join-Path $env:WINDIR 'explorer.exe'
$shortcut.Arguments = 'shell:AppsFolder\' + $installed.PackageFamilyName + '!App'
$shortcut.WorkingDirectory = $layoutRoot
$shortcut.IconLocation = (Join-Path $layoutRoot 'AeluraTool.exe') + ',0'
$shortcut.Save()
$launcher = '@echo off' + "`r`n" + 'start "" "%SystemRoot%\explorer.exe" "shell:AppsFolder\' + $installed.PackageFamilyName + '!App"' + "`r`n"
Set-Content -LiteralPath (Join-Path $outputRoot 'Start-AeluraTool.cmd') -Value $launcher -Encoding Ascii
Write-Output "Deployed $Configuration layout."
