$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'install_smoke.ps1'
$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
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
Write-Output 'PASS: installation harness parses and rejects non-CI execution before accessing artifacts or changing the system.'
