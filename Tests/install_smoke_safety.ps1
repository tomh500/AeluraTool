$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'install_smoke.ps1'
$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
. (Join-Path $PSScriptRoot 'Installer.BatRunner.ps1')
$originalActions = $env:GITHUB_ACTIONS
try {
    $env:GITHUB_ACTIONS = $null
    $rejected = $false
    try { & $script -ArtifactDirectory 'must-not-be-read' -ReportDirectory 'must-not-be-created' }
    catch {
        if ($_.Exception.Message -ne 'Installation smoke tests require a disposable GitHub-hosted Windows runner.') { throw }
        $rejected = $true
    }
    if (!$rejected) { throw 'Installation smoke test accepted a non-CI host.' }
} finally { $env:GITHUB_ACTIONS = $originalActions }
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = [IO.Path]::GetFullPath((Join-Path $temporaryRoot ('Aelura BAT fixture ' + [guid]::NewGuid().ToString('N'))))
if (!$fixture.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid fixture directory.' }
New-Item -ItemType Directory -Path $fixture | Out-Null
$originalInputEncoding = [Console]::InputEncoding
try {
    # Reproduce the hosted runner's UTF-8 input encoding, including its BOM.
    [Console]::InputEncoding = New-Object Text.UTF8Encoding($true)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../scripts/Install.bat') -Destination $fixture
    @'
param([switch]$NoPause)
if (!$NoPause -or (Read-Host 'Fixture consent') -notmatch '^(y|yes)$') { exit 98 }
Write-Output 'CONSENT_RECEIVED'
exit 0
'@ | Set-Content -LiteralPath (Join-Path $fixture 'Install.ps1') -Encoding UTF8
    $log = Join-Path $fixture 'installer.log'
    if ((Invoke-InstallerBat $fixture $log 30000) -ne 0 -or
        (Get-Content -LiteralPath $log -Raw) -notmatch 'CONSENT_RECEIVED') { throw 'The real BAT process driver corrupted consent input.' }
    'param([switch]$NoPause); exit 23' | Set-Content -LiteralPath (Join-Path $fixture 'Install.ps1') -Encoding UTF8
    if ((Invoke-InstallerBat $fixture $log 30000) -ne 23) { throw 'BAT failure exit code was lost.' }
} finally {
    [Console]::InputEncoding = $originalInputEncoding
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
Write-Output 'PASS: installation harness parses and rejects non-CI execution before accessing artifacts or changing the system.'
Write-Output 'PASS: shared Windows PowerShell BAT driver sends exact consent and preserves failure exit codes.'
