param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$outputRoot = Join-Path $repoRoot "x64\$Configuration\AeluraTool"
$recipePath = Join-Path $outputRoot 'AeluraTool.build.appxrecipe'
if (!(Test-Path -LiteralPath $recipePath)) { throw 'Build this configuration first.' }
[xml]$recipe = Get-Content -LiteralPath $recipePath -Raw
$layoutRoot = [IO.Path]::GetFullPath((Join-Path $outputRoot 'AppX'))
foreach ($item in $recipe.Project.ItemGroup.ChildNodes) {
    if (!$item.PackagePath -or !$item.Include) { continue }
    $targetPath = [IO.Path]::GetFullPath((Join-Path $layoutRoot $item.PackagePath))
    if (!$targetPath.StartsWith($layoutRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package layout path.' }
    New-Item -ItemType Directory -Path (Split-Path $targetPath -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $item.Include -Destination $targetPath -Force
}
Add-AppxPackage -Register (Join-Path $layoutRoot 'AppxManifest.xml')
Write-Output "Deployed $Configuration layout."
