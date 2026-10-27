function Invoke-InstallerBat([string]$Directory, [string]$LogPath, [int]$TimeoutMilliseconds = 180000) {
    $responseName = 'aelura-ci-consent-' + [guid]::NewGuid().ToString('N') + '.txt'
    $responsePath = Join-Path $Directory $responseName
    [IO.File]::WriteAllText($responsePath, "yes`r`n`r`n", [Text.Encoding]::ASCII)
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    # Avoid .NET's redirected-input StreamWriter entirely: on UTF-8 runners
    # it writes a BOM while Process.Start initializes AutoFlush, before any
    # caller can write raw bytes. CMD receives a plain ASCII response file.
    $info.Arguments = '/d /c call Install.bat < ' + $responseName
    $info.WorkingDirectory = $Directory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (!$process.Start()) { throw 'Could not start Install.bat.' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($TimeoutMilliseconds)) {
            & "$env:WINDIR\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
            throw 'Install.bat exceeded its time limit.'
        }
        ($output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()) |
            Set-Content -LiteralPath $LogPath -Encoding UTF8
        return $process.ExitCode
    } finally {
        $process.Dispose()
        Remove-Item -LiteralPath $responsePath -Force -ErrorAction SilentlyContinue
    }
}
