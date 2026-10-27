# Read package metadata without extracting or modifying package contents.
function Read-InstallerManifest([string]$Path) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    $reader = $null
    try {
        $entry = $archive.GetEntry('AppxManifest.xml')
        if (!$entry) { throw 'install.package_invalid' }
        $settings = New-Object Xml.XmlReaderSettings
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create($entry.Open(), $settings)
        $document = New-Object Xml.XmlDocument
        $document.XmlResolver = $null
        $document.Load($reader)
        if (!$document.Package.Identity) { throw 'install.package_invalid' }
        return $document
    } finally {
        if ($reader) { $reader.Dispose() }
        $archive.Dispose()
    }
}

function Test-InstallerDependencySatisfied($Requirement, [object[]]$InstalledPackages) {
    foreach ($installed in $InstalledPackages) {
        if ($installed.Name -eq $Requirement.Name -and $installed.Publisher -eq $Requirement.Publisher -and
            [string]$installed.Architecture -in @($Requirement.Architecture, 'neutral') -and
            [version]$installed.Version -ge [version]$Requirement.MinVersion -and
            $installed.IsFramework -and [string]$installed.Status -eq 'Ok') { return $true }
    }
    return $false
}

function Get-InstallerDependencyPlan([string]$PackagePath, [string[]]$DependencyPaths, [object[]]$InstalledPackages) {
    $manifest = Read-InstallerManifest $PackagePath
    $candidates = @($DependencyPaths | ForEach-Object {
        $metadata = Read-InstallerManifest $_
        [pscustomobject]@{ Path = $_; Identity = $metadata.Package.Identity; Framework = $metadata.Package.Properties.Framework }
    })
    $requirements = @()
    $paths = @()
    foreach ($dependency in $manifest.Package.Dependencies.PackageDependency) {
        $architecture = if ($dependency.ProcessorArchitecture) { [string]$dependency.ProcessorArchitecture } else { [string]$manifest.Package.Identity.ProcessorArchitecture }
        $requirement = [pscustomobject]@{
            Name = [string]$dependency.Name
            Publisher = [string]$dependency.Publisher
            MinVersion = [string]$dependency.MinVersion
            Architecture = $architecture
            Satisfied = $false
        }
        $candidate = $candidates | Where-Object {
            $_.Identity.Name -eq $requirement.Name -and $_.Identity.Publisher -eq $requirement.Publisher -and
            $_.Identity.ProcessorArchitecture -in @($architecture, 'neutral') -and
            [version]$_.Identity.Version -ge [version]$requirement.MinVersion -and $_.Framework -eq 'true'
        } | Sort-Object { [version]$_.Identity.Version } | Select-Object -First 1
        # Validate the archive remains usable on a machine without any dependencies.
        if (!$candidate) { throw 'install.dependencies_missing' }
        $requirement.Satisfied = Test-InstallerDependencySatisfied $requirement $InstalledPackages
        $requirements += $requirement
        if (!$requirement.Satisfied) { $paths += $candidate.Path }
    }
    return [pscustomobject]@{ PackagePath = $PackagePath; Identity = $manifest.Package.Identity; Requirements = $requirements; Paths = $paths }
}

function Invoke-InstallerDeployment($Plan) {
    $parameters = @{ Path = $Plan.PackagePath; DeferRegistrationWhenPackagesAreInUse = $true; ErrorAction = 'Stop' }
    if ($Plan.Paths.Count) { $parameters.DependencyPath = [string[]]$Plan.Paths }
    Add-AppxPackage @parameters
    # Deferred registration may return before this user can run the application.
    # Never create a shortcut or report success until the requested version and
    # all its dependencies are registered and healthy for this user.
    $installed = Get-AppxPackage -Name $Plan.Identity.Name | Where-Object {
        $_.Publisher -eq $Plan.Identity.Publisher -and
        [string]$_.Architecture -eq [string]$Plan.Identity.ProcessorArchitecture -and
        [version]$_.Version -ge [version]$Plan.Identity.Version -and
        [string]$_.Status -eq 'Ok' -and !$_.IsDevelopmentMode
    } | Sort-Object { [version]$_.Version } -Descending | Select-Object -First 1
    $complete = $null -ne $installed
    foreach ($requirement in $Plan.Requirements) {
        if (!(Test-InstallerDependencySatisfied $requirement @(Get-AppxPackage -Name $requirement.Name))) { $complete = $false }
    }
    return [pscustomobject]@{ Complete = $complete; InstalledPackage = $installed }
}
