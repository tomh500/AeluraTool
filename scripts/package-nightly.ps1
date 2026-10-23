# CI inputs are secrets; never print them or pass passwords on a command line.
$ErrorActionPreference = 'Stop'
if (!$env:CI -or !$env:RUNNER_TEMP) { throw 'Run this script on the CI runner.' }
if (!$env:MSIX_SIGNING_PFX_BASE64 -or !$env:MSIX_SIGNING_PFX_PASSWORD) {
    throw 'Configure repository secrets MSIX_SIGNING_PFX_BASE64 and MSIX_SIGNING_PFX_PASSWORD before publishing nightly packages.'
}
$repoRoot = Split-Path $PSScriptRoot -Parent
$pfxPath = Join-Path $env:RUNNER_TEMP ("aelura-signing-" + [guid]::NewGuid().ToString('N') + '.pfx')
$certificate = $null
try {
    [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($env:MSIX_SIGNING_PFX_BASE64))
    $password = ConvertTo-SecureString $env:MSIX_SIGNING_PFX_PASSWORD -AsPlainText -Force
    $certificate = @(Import-PfxCertificate -FilePath $pfxPath -Password $password -CertStoreLocation Cert:\CurrentUser\My)
    if ($certificate.Count -ne 1 -or !$certificate[0].HasPrivateKey) { throw 'The signing PFX must contain exactly one signing certificate with its private key.' }
    & (Join-Path $PSScriptRoot 'distribute.ps1') -CertificateThumbprint $certificate[0].Thumbprint
    $archives = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'artifacts\distribution') -Filter '*.zip' -File)
    if ($archives.Count -ne 1) { throw 'Expected exactly one signed distribution archive on a clean runner.' }
    python (Join-Path $repoRoot 'Tests\check_distribution.py') $archives[0].FullName
    if ($LASTEXITCODE -ne 0) { throw 'Extracted distribution checks failed; nightly was not published.' }
    if ($env:NIGHTLY_NAME -notmatch '^AeluraTool-nightly-[0-9-]+-x64$') { throw 'Invalid nightly archive name.' }
    $nightly = Join-Path $repoRoot 'artifacts\nightly'
    New-Item -ItemType Directory -Path $nightly -Force | Out-Null
    $archive = Join-Path $nightly ($env:NIGHTLY_NAME + '.zip')
    Copy-Item -LiteralPath $archives[0].FullName -Destination $archive
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    "$hash  $env:NIGHTLY_NAME.zip" | Set-Content -LiteralPath (Join-Path $nightly ($env:NIGHTLY_NAME + '.sha256')) -Encoding UTF8
} finally {
    if (Test-Path -LiteralPath $pfxPath) { Remove-Item -LiteralPath $pfxPath -Force }
    foreach ($item in $certificate) {
        Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $item.Thumbprint) -DeleteKey -Force
    }
    $env:MSIX_SIGNING_PFX_BASE64 = $null
    $env:MSIX_SIGNING_PFX_PASSWORD = $null
}
