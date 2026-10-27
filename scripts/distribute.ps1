param([Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$CertificateThumbprint)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
[xml]$manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'Package.appxmanifest') -Raw
$certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if (!$certificate.HasPrivateKey -or $certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date)) { throw 'Signing certificate must have a private key and be within its validity period.' }
if ($certificate.Subject -ne $manifest.Package.Identity.Publisher) { throw 'Signing certificate subject does not match the package publisher.' }
& (Join-Path $PSScriptRoot 'package.ps1') -CertificateThumbprint $CertificateThumbprint
$version = $manifest.Package.Identity.Version
$source = Join-Path $repoRoot "artifacts\packages\AeluraTool_${version}_x64_Test"
$package = Join-Path $source "AeluraTool_${version}_x64.msix"
$signature = Get-AuthenticodeSignature -LiteralPath $package
if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $CertificateThumbprint -or $signature.Status -notin @('Valid', 'NotTrusted', 'UnknownError')) { throw 'Package signature is missing, damaged or does not match the selected certificate.' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$folder = Join-Path $repoRoot "artifacts\distribution\AeluraTool-${version}-x64-$stamp"
New-Item -ItemType Directory -Path $folder | Out-Null
Copy-Item -LiteralPath $package -Destination $folder
foreach ($name in @('Add-AppDevPackage.ps1', 'Add-AppDevPackage.resources')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $folder -Recurse
}
$dependencies = Join-Path $folder 'Dependencies'
New-Item -ItemType Directory -Path $dependencies | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'Dependencies\x64') -Destination $dependencies -Recurse
Export-Certificate -Cert $certificate -FilePath (Join-Path $folder 'AeluraTool.cer') | Out-Null
$installer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Install.ps1') -Raw
$installer.Replace('__SIGNER_THUMBPRINT__', $certificate.Thumbprint) | Set-Content -LiteralPath (Join-Path $folder 'Install.ps1') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install.cmd') -Destination $folder
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install.bat') -Destination $folder
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Installer.Dependencies.ps1') -Destination $folder
Copy-Item -LiteralPath (Join-Path $repoRoot 'Strings') -Destination $folder -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'Tools\Licenses') -Destination $folder -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'BUILD_AND_DISTRIBUTE.md') -Destination $folder
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DISTRIBUTION_README.txt') -Destination (Join-Path $folder 'README.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'Docs\Installing.md') -Destination (Join-Path $folder 'INSTALLING.md')
& (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File (Join-Path $folder 'Install.ps1') -CheckOnly
if ($LASTEXITCODE -ne 0) { throw 'Distribution installer preflight failed; archive was not created.' }
$archive = "$folder.zip"
Compress-Archive -LiteralPath $folder -DestinationPath $archive
Get-FileHash -LiteralPath $archive -Algorithm SHA256 | Format-List
Write-Output "Distribution archive: $archive"
