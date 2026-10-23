param([switch]$CheckOnly, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$exitCode = 0
$language = if ([Globalization.CultureInfo]::CurrentUICulture.Name -like 'zh-*') { 'zh-CN' } else { 'en-US' }
$strings = Get-Content -LiteralPath (Join-Path $PSScriptRoot "Strings\$language.json") -Raw -Encoding UTF8 | ConvertFrom-Json
function T([string]$key) { $strings.$key }
try {
    Write-Host (T 'install.title') -ForegroundColor Cyan
    if (![Environment]::Is64BitOperatingSystem -or ![Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw (T 'install.x64') }
    $packages = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.msix' -File)
    if ($packages.Count -ne 1) { throw (T 'install.package_missing') }
    $package = $packages[0]
    $certificatePath = Join-Path $PSScriptRoot 'AeluraTool.cer'
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
    $signature = Get-AuthenticodeSignature -LiteralPath $package.FullName
    # The distributor pins the selected signing certificate when assembling the ZIP.
    $expected = '__SIGNER_THUMBPRINT__'
    if ($certificate.Thumbprint -ne $expected -or !$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $expected -or $signature.Status -notin @('Valid', 'NotTrusted', 'UnknownError')) { throw (T 'install.signature_failed') }
    Write-Host ((T 'install.signer') -f $certificate.Subject, $certificate.Thumbprint)
    $dependencies = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Dependencies\x64') -File | Where-Object Extension -in @('.appx','.msix'))
    if (!$dependencies.Count) { throw (T 'install.dependencies_missing') }
    if ($CheckOnly) { Write-Host (T 'install.check_ok'); exit 0 }
    if (Get-Process AeluraTool, StrikeSense -ErrorAction SilentlyContinue) { throw (T 'install.close') }
    if (!(Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$expected")) {
        Write-Host (T 'install.trust_hint') -ForegroundColor Yellow
        if ((Read-Host (T 'install.trust_question')) -notmatch '^(y|yes)$') { throw (T 'install.cancelled') }
        # Elevate only certificate import. App installation stays with the original user.
        $escapedPath = $certificatePath.Replace("'", "''")
        $command = @"
`$ErrorActionPreference = 'Stop'
try {
    `$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2('$escapedPath')
    if (`$cert.Thumbprint -ne '$expected') { exit 1 }
    Import-Certificate -FilePath '$escapedPath' -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
    exit 0
} catch { exit 1 }
"@
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        $helper = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile','-EncodedCommand',$encoded) -Wait -PassThru
        if ($helper.ExitCode -ne 0) { throw (T 'install.trust_failed') }
    }
    # AppX deployment validates package integrity and TrustedPeople trust itself.
    # WinVerifyTrust may still report UnknownError for a TrustedPeople self-signed
    # publisher: do not require adding this certificate to the root store.
    Write-Host (T 'install.installing')
    Add-AppxPackage -Path $package.FullName -DependencyPath @($dependencies.FullName)
    $installed = Get-AppxPackage -Name '27abd2dc-3cdf-47a6-a74b-7cae8037deaa'
    if (!$installed) { throw (T 'install.not_found') }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) ((T 'install.shortcut') + '.lnk')))
    $link.TargetPath = Join-Path $env:WINDIR 'explorer.exe'
    $link.Arguments = 'shell:AppsFolder\' + $installed.PackageFamilyName + '!App'
    $link.WorkingDirectory = $installed.InstallLocation
    $link.IconLocation = (Join-Path $installed.InstallLocation 'AeluraTool.exe') + ',0'
    $link.Save()
    Write-Host (T 'install.success') -ForegroundColor Green
} catch {
    $exitCode = 1
    Write-Host ((T 'install.failure') -f $_.Exception.Message) -ForegroundColor Red
    Write-Host (T 'install.help')
} finally {
    if (!$CheckOnly -and !$NoPause) { Read-Host (T 'install.finish') | Out-Null }
}
exit $exitCode
