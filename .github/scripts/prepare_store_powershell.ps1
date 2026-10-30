param([Parameter(Mandatory = $true)][string]$EvidenceDirectory)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$null = New-Item -ItemType Directory -Path $EvidenceDirectory -Force
$evidencePath = Join-Path $EvidenceDirectory 'msix-install.json'
$elapsed = [Diagnostics.Stopwatch]::StartNew()
$evidence = [ordered]@{
    status = 'pending'
    phase = 'starting'
    started_at = [DateTime]::UtcNow.ToString('o')
    elapsed_seconds = 0
    release = $null
    asset_url = $null
    sha256 = $null
    package = $null
    executable = $null
    reason = $null
}

function Save-SetupEvidence {
    $evidence.elapsed_seconds = [Math]::Round($elapsed.Elapsed.TotalSeconds, 2)
    $evidence | ConvertTo-Json | Set-Content -LiteralPath "$evidencePath.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$evidencePath.tmp" -Destination $evidencePath -Force
}

function Set-SetupPhase([string]$Phase) {
    $evidence.phase = $Phase
    Save-SetupEvidence
    Write-Host "Store PowerShell MSIX phase: $Phase ($($evidence.elapsed_seconds)s)"
}

try {
    # Resolve the current stable release, not a frozen PowerShell version.
    # Add-AppxPackage verifies the published bundle's signature at installation.
    Set-SetupPhase 'release lookup (60s limit)'
    $release = Invoke-RestMethod 'https://api.github.com/repos/PowerShell/PowerShell/releases/latest' -TimeoutSec 60
    $assets = @($release.assets | Where-Object name -Match '^PowerShell-[0-9.]+\.msixbundle$')
    if ($assets.Count -ne 1) {
        throw 'The current stable PowerShell release does not publish exactly one MSIX bundle.'
    }
    $evidence.release = $release.tag_name
    $evidence.asset_url = $assets[0].browser_download_url
    $bundle = Join-Path $env:RUNNER_TEMP $assets[0].name
    Set-SetupPhase 'bundle download (300s limit)'
    Invoke-WebRequest -Uri $evidence.asset_url -OutFile $bundle -UseBasicParsing -TimeoutSec 300
    $evidence.sha256 = (Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-SetupPhase 'package installation (120s limit)'
    $installJob = Start-Job -ScriptBlock {
        param($BundlePath)
        $ErrorActionPreference = 'Stop'
        Add-AppxPackage -Path $BundlePath -ErrorAction Stop
    } -ArgumentList $bundle
    try {
        if (-not (Wait-Job -Job $installJob -Timeout 120)) {
            throw 'MSIX installation exceeded 120 seconds.'
        }
        Receive-Job -Job $installJob -ErrorAction Stop | Out-Null
    }
    finally {
        if ($installJob.State -eq 'Running') {
            Stop-Job -Job $installJob
        }
        Remove-Job -Job $installJob -Force
    }
    Set-SetupPhase 'package registration check'
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
    $evidence.status = 'unavailable'
    $evidence.reason = $_.Exception.Message
    Write-Warning "Store PowerShell MSIX setup unavailable: $($evidence.reason)"
}

Save-SetupEvidence
$evidence | ConvertTo-Json
"Store PowerShell MSIX setup: **$($evidence.status)**. $($evidence.reason)" |
    Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
