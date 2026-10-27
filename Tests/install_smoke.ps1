param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$ReportDirectory
)
$ErrorActionPreference = 'Stop'
# This test changes package registration and certificate trust. It must never
# run on a developer machine or a persistent self-hosted runner.
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or !$env:RUNNER_TEMP) {
    throw 'Installation smoke tests require a disposable GitHub-hosted Windows runner.'
}
if (![Environment]::Is64BitProcess -or $PSVersionTable.PSEdition -ne 'Desktop') { throw 'Use 64-bit Windows PowerShell.' }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'The disposable runner must be elevated for the installer trust prompt.' }
$reports = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Path $reports -Force | Out-Null
$report = [ordered]@{
    passed = $false
    scope = 'Fresh GitHub-hosted Windows VM, separate from build; not an unmodified retail Windows image.'
    os = [Environment]::OSVersion.VersionString
    initialDependencies = @()
    installedDependencies = @()
    sourceRun = $env:INSTALL_SOURCE_RUN
    sourceRevision = $env:INSTALL_SOURCE_REVISION
    error = $null
}
$scratch = Join-Path $env:RUNNER_TEMP ('Aelura install smoke ' + [guid]::NewGuid().ToString('N'))
$packageName = $null
$thumbprint = $null
$appProcess = $null
$packageOwned = $false
$trustOwned = $false
$failure = $null
$started = Get-Date
. (Join-Path $PSScriptRoot 'Installer.BatRunner.ps1')
try {
    $archives = @(Get-ChildItem -LiteralPath $ArtifactDirectory -Filter '*.zip' -File)
    if ($archives.Count -ne 1) { throw 'Expected one signed release ZIP.' }
    $archive = $archives[0]
    $checksum = [IO.Path]::ChangeExtension($archive.FullName, '.sha256')
    $expectedHash = ((Get-Content -LiteralPath $checksum -Raw).Trim() -split '\s+')[0]
    $actualHash = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash
    if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$' -or $actualHash -ne $expectedHash) { throw 'Release ZIP checksum mismatch.' }
    $report.archiveSha256 = $actualHash
    New-Item -ItemType Directory -Path $scratch | Out-Null
    Expand-Archive -LiteralPath $archive.FullName -DestinationPath $scratch
    $launchers = @(Get-ChildItem -LiteralPath $scratch -Filter 'Install.bat' -Recurse -File)
    if ($launchers.Count -ne 1) { throw 'Expected one user Install.bat in the archive.' }
    $folder = $launchers[0].DirectoryName
    if ((Get-FileHash -LiteralPath $launchers[0].FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../scripts/Install.bat')).Hash) {
        throw 'The archived BAT differs from the checked-out user launcher.'
    }
    . (Join-Path $folder 'Installer.Dependencies.ps1')
    $packages = @(Get-ChildItem -LiteralPath $folder -Filter '*.msix' -File)
    if ($packages.Count -ne 1) { throw 'Expected one MSIX.' }
    $manifest = Read-InstallerManifest $packages[0].FullName
    $packageName = [string]$manifest.Package.Identity.Name
    if (Get-AppxPackage -Name $packageName) { throw 'The test application is already registered; this is not a fresh installation.' }
    $packageOwned = $true
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $folder 'AeluraTool.cer'))
    $thumbprint = $certificate.Thumbprint
    if ((Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$thumbprint") -or
        (Test-Path -LiteralPath "Cert:\CurrentUser\TrustedPeople\$thumbprint")) { throw 'The signing certificate is already trusted.' }
    $trustOwned = $true
    $report.signer = $thumbprint
    $dependencies = @(Get-ChildItem -LiteralPath (Join-Path $folder 'Dependencies/x64') -File | Where-Object Extension -in @('.appx', '.msix'))
    $plan = Get-InstallerDependencyPlan $packages[0].FullName @($dependencies.FullName) @(Get-AppxPackage)
    $report.initialDependencies = @($plan.Requirements)
    if (!$plan.Paths.Count) { throw 'All runtimes are already installed; missing-dependency installation would not be tested.' }
    # Exercise the user prompt/elevation helper without CheckOnly, direct
    # Add-AppxPackage, or a pre-imported certificate.
    $report.installerExitCode = Invoke-InstallerBat $folder (Join-Path $reports 'installer.log')
    if ($report.installerExitCode -ne 0) { throw "Install.bat returned $($report.installerExitCode); see installer.log." }
    $installed = @(Get-AppxPackage -Name $packageName | Where-Object { [string]$_.Status -eq 'Ok' -and !$_.IsDevelopmentMode })
    if ($installed.Count -ne 1 -or $installed[0].Publisher -ne $manifest.Package.Identity.Publisher -or
        [version]$installed[0].Version -ne [version]$manifest.Package.Identity.Version) { throw 'Installed package identity/version/status does not match the release.' }
    $installed = $installed[0]
    $report.packageFullName = $installed.PackageFullName
    foreach ($requirement in $plan.Requirements) {
        $actual = @(Get-AppxPackage -Name $requirement.Name)
        if (!(Test-InstallerDependencySatisfied $requirement $actual)) { throw "Missing registered runtime: $($requirement.Name)" }
        $report.installedDependencies += @($actual | Select-Object Name, Version, Architecture, Status)
    }
    if (!(Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$thumbprint")) { throw 'The BAT did not complete certificate trust.' }
    $shell = New-Object -ComObject WScript.Shell
    $aumid = $installed.PackageFamilyName + '!App'
    $links = @(Get-ChildItem -LiteralPath ([Environment]::GetFolderPath('Desktop')) -Filter '*.lnk' -File | ForEach-Object {
        $link = $shell.CreateShortcut($_.FullName)
        if ($link.Arguments -eq "shell:AppsFolder\$aumid") { $link }
    })
    if ($links.Count -ne 1 -or $links[0].TargetPath -ne (Join-Path $env:WINDIR 'explorer.exe') -or
        $links[0].WorkingDirectory -ne $installed.InstallLocation -or
        $links[0].IconLocation -ne ((Join-Path $installed.InstallLocation 'AeluraTool.exe') + ',0')) { throw 'Installed desktop shortcut is incorrect.' }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IInstallSmokeActivation {
    [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string id, [MarshalAs(UnmanagedType.LPWStr)] string args, uint options, out uint pid);
}
public static class InstallSmokeActivation {
    public static uint Start(string id) {
        object manager = Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")));
        try { uint pid; Marshal.ThrowExceptionForHR(((IInstallSmokeActivation)manager).ActivateApplication(id, null, 0, out pid)); return pid; }
        finally { Marshal.ReleaseComObject(manager); }
    }
}
public static class InstallSmokeWindow {
    delegate bool EnumCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    public static IntPtr Find(uint process) {
        IntPtr found=IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if(owner==process && GetProp(window, "AeluraTool.MainWindow")!=IntPtr.Zero && IsWindowVisible(window) && IsWindowEnabled(window)) { found=window; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static void Show(IntPtr window) {
        ShowWindow(window, 9);
        SetWindowPos(window, new IntPtr(-1), 0, 0, 0, 0, 0x0041);
        SetForegroundWindow(window);
    }
    public static bool Responds(IntPtr window) {
        IntPtr result; return SendMessageTimeout(window, 0, IntPtr.Zero, IntPtr.Zero, 2, 5000, out result)!=IntPtr.Zero;
    }
    public static bool Close(IntPtr window) { return PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
'@
    $appProcess = Get-Process -Id ([InstallSmokeActivation]::Start($aumid))
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 250
        $appProcess.Refresh()
        if ($appProcess.HasExited) { throw 'The installed application exited during startup.' }
        $mainWindow = [InstallSmokeWindow]::Find($appProcess.Id)
    } while ($mainWindow -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($mainWindow -eq [IntPtr]::Zero -or $appProcess.MainModule.FileName -ne (Join-Path $installed.InstallLocation 'AeluraTool.exe')) { throw 'No tagged visible enabled main window from the installed package appeared.' }
    [InstallSmokeWindow]::Show($mainWindow)
    Start-Sleep -Seconds 5
    $appProcess.Refresh()
    if ($appProcess.HasExited -or ![InstallSmokeWindow]::Responds($mainWindow)) { throw 'The installed main window did not remain responsive.' }
    $report.activation = 'Package identity activation produced a responsive main window for at least five seconds.'
    try {
        Add-Type -AssemblyName System.Drawing, System.Windows.Forms
        $bounds = [Windows.Forms.SystemInformation]::VirtualScreen
        $bitmap = New-Object Drawing.Bitmap($bounds.Width, $bounds.Height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size); $bitmap.Save((Join-Path $reports 'installed-app.png')) }
        finally { $graphics.Dispose(); $bitmap.Dispose() }
    } catch { $report.screenshotLimitation = $_.Exception.Message }
    if (![InstallSmokeWindow]::Close($mainWindow)) { throw 'Could not request closing the installed main window.' }
    # A fresh profile asks whether to exit or minimize to the tray. Choose the
    # real Exit button through UI Automation, scoped to this package's window.
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $exitNames = @('en-US', 'zh-CN' | ForEach-Object {
        (Get-Content -LiteralPath (Join-Path $folder "Strings/$_.json") -Raw -Encoding UTF8 | ConvertFrom-Json).'close.exit'
    })
    $exitDeadline = (Get-Date).AddSeconds(15)
    $exitInvoked = $false
    do {
        $appProcess.Refresh()
        if ($appProcess.HasExited) { break }
        $window = [Windows.Automation.AutomationElement]::FromHandle($mainWindow)
        $condition = New-Object Windows.Automation.AndCondition(
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $appProcess.Id)),
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
        foreach ($button in $window.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
            if ($button.Current.Name -in $exitNames -and $button.Current.IsEnabled) {
                $invoke = [Windows.Automation.InvokePattern]$button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)
                $invoke.Invoke()
                $exitInvoked = $true
                break
            }
        }
        if (!$exitInvoked) { Start-Sleep -Milliseconds 250 }
    } while (!$exitInvoked -and (Get-Date) -lt $exitDeadline)
    if (!$exitInvoked -or !$appProcess.WaitForExit(20000)) { throw 'The first-run Exit confirmation did not close the idle application.' }
    $report.shutdown = 'Selected Exit in the first-run close confirmation and observed normal process termination.'
    $report.passed = $true
} catch {
    $failure = $_
    $report.error = $_.Exception.Message
} finally {
    if ($appProcess -and !$appProcess.HasExited) { Stop-Process -Id $appProcess.Id -Force -ErrorAction SilentlyContinue }
    if ($packageOwned) {
        Get-AppxPackage -Name $packageName | Remove-AppxPackage -ErrorAction Continue
    }
    if ($trustOwned -and (Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$thumbprint")) {
        Remove-Item -LiteralPath "Cert:\LocalMachine\TrustedPeople\$thumbprint" -Force
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $reports 'install-report.json') -Encoding UTF8
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $started; Level = @(1, 2) } -ErrorAction Stop |
            Select-Object TimeCreated, ProviderName, Message | ConvertTo-Json -Depth 3 |
            Set-Content -LiteralPath (Join-Path $reports 'application-errors.json') -Encoding UTF8
    } catch { }
    if ($env:GITHUB_STEP_SUMMARY) {
        @('## Windows installation smoke test', '', "Passed: $($report.passed)", '',
          'Ran the distributed Install.bat with certificate consent on a separate fresh hosted Windows VM.',
          'Verified runtime deployment, package registration, desktop shortcut and package-identity GUI activation.', '',
          'Scope: the GitHub image has preinstalled OS/development software; it is not a pristine retail Windows VM.',
          "Details: $($report.error)") | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding UTF8
    }
}
if ($failure) { throw $failure }
Write-Output 'PASS: real BAT installation, missing runtimes, certificate trust, shortcut and packaged application startup.'
