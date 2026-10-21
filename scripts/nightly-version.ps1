param([datetime]$Timestamp = [datetime]::UtcNow)
$ErrorActionPreference = 'Stop'
$utc = $Timestamp.ToUniversalTime()
$stamp = $utc.ToString('yyyyMMdd-HHmmss')
# MSIX components must fit UInt16; keep the Store-reserved revision at zero.
$version = '{0}.{1}.{2}.0' -f $utc.Year, $utc.DayOfYear, ($utc.Hour * 60 + $utc.Minute)
$path = Join-Path (Split-Path $PSScriptRoot -Parent) 'Package.appxmanifest'
$document = [xml](Get-Content -LiteralPath $path -Raw)
$document.Package.Identity.Version = $version
$document.Save($path)
if ($env:GITHUB_OUTPUT) {
    "stamp=$stamp" | Out-File $env:GITHUB_OUTPUT -Append -Encoding utf8
    "version=$version" | Out-File $env:GITHUB_OUTPUT -Append -Encoding utf8
}
Write-Output "Nightly UTC: $stamp; MSIX: $version"
