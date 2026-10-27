function Invoke-InstallerBat([string]$Directory, [string]$LogPath, [int]$TimeoutMilliseconds = 180000) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = '/d /c call Install.bat'
    $info.WorkingDirectory = $Directory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (!$process.Start()) { throw 'Could not start Install.bat.' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        # Windows PowerShell's StreamWriter emits a UTF-8 BOM. Write ASCII to
        # the underlying pipe so Read-Host receives exactly "yes", as typed.
        $inputBytes = [Text.Encoding]::ASCII.GetBytes("yes`r`n`r`n")
        try {
            $process.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length)
            $process.StandardInput.BaseStream.Flush()
        } catch [IO.IOException] { }
        $process.StandardInput.BaseStream.Close()
        if (!$process.WaitForExit($TimeoutMilliseconds)) {
            & "$env:WINDIR\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
            throw 'Install.bat exceeded its time limit.'
        }
        ($output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()) |
            Set-Content -LiteralPath $LogPath -Encoding UTF8
        return $process.ExitCode
    } finally { $process.Dispose() }
}
