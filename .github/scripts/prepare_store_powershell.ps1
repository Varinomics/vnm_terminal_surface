param([Parameter(Mandatory = $true)][string]$EvidenceDirectory)

$ErrorActionPreference = 'Stop'
$null = New-Item -ItemType Directory -Path $EvidenceDirectory -Force
$evidence = [ordered]@{
    status = 'unavailable'
    release = $null
    asset_url = $null
    sha256 = $null
    package = $null
    executable = $null
    reason = $null
}

try {
    # Resolve the current stable release, not a frozen PowerShell version.
    # Add-AppxPackage verifies the published bundle's signature at installation.
    $release = Invoke-RestMethod 'https://api.github.com/repos/PowerShell/PowerShell/releases/latest'
    $assets = @($release.assets | Where-Object name -Match '^PowerShell-[0-9.]+\.msixbundle$')
    if ($assets.Count -ne 1) {
        throw 'The current stable PowerShell release does not publish exactly one MSIX bundle.'
    }
    $evidence.release = $release.tag_name
    $evidence.asset_url = $assets[0].browser_download_url
    $bundle = Join-Path $env:RUNNER_TEMP $assets[0].name
    Invoke-WebRequest -Uri $evidence.asset_url -OutFile $bundle -UseBasicParsing
    $evidence.sha256 = (Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
    Add-AppxPackage -Path $bundle -ErrorAction Stop
    $packages = @(Get-AppxPackage -Name Microsoft.PowerShell | Where-Object {
        $_.PackageFamilyName -eq 'Microsoft.PowerShell_8wekyb3d8bbwe' -and
        (Test-Path -LiteralPath (Join-Path $_.InstallLocation 'pwsh.exe') -PathType Leaf)
    })
    if ($packages.Count -eq 0) {
        throw 'MSIX installation returned without a registered Microsoft.PowerShell executable.'
    }
    $evidence.package = $packages[0].PackageFullName
    $evidence.executable = Join-Path $packages[0].InstallLocation 'pwsh.exe'
    $evidence.status = 'installed'
}
catch {
    $evidence.reason = $_.Exception.Message
    Write-Warning "Store PowerShell MSIX setup unavailable: $($evidence.reason)"
}

$evidence | ConvertTo-Json | Tee-Object -FilePath (Join-Path $EvidenceDirectory 'msix-install.json')
"Store PowerShell MSIX setup: **$($evidence.status)**. $($evidence.reason)" |
    Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
