param(
    [string]$TestExecutable,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [ValidateSet('store-root', 'packaged-descendants')][string]$Scenario = 'store-root',
    [switch]$Worker
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 -File initializes PSScriptRoot after parameter defaults.
if (-not $PSBoundParameters.ContainsKey('TestExecutable')) {
    $TestExecutable = Join-Path $PSScriptRoot 'vnm_terminal_windows_conpty_backend.exe'
}
$TestExecutable = (Resolve-Path -LiteralPath $TestExecutable).Path
$null = New-Item -ItemType Directory -Path $EvidenceDirectory -Force
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$resultPath = Join-Path $EvidenceDirectory "$Scenario-result.json"
$logPath = Join-Path $EvidenceDirectory "$Scenario.log"
$testArguments = if ($Scenario -eq 'store-root') { '--store-powershell-root' } else { '--packaged-descendants' }
$timeoutSeconds = if ($Scenario -eq 'store-root') { 100 } else { 300 }

if ($Worker) {
    # The deployed runtime is self-contained; WMI need not inherit a Qt PATH.
    $errorPath = Join-Path $EvidenceDirectory "$Scenario.stderr.log"
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $TestExecutable
    $process.StartInfo.Arguments = $testArguments
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = $false
        if ($process.WaitForExit($timeoutSeconds * 1000)) {
            $testExitCode = $process.ExitCode
        }
        else {
            $process.Kill()
            $process.WaitForExit()
            $testExitCode = 1
            $timedOut = $true
        }
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath $logPath -Encoding UTF8
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath $errorPath -Encoding UTF8
        if ($timedOut) {
            "$Scenario test exceeded $timeoutSeconds seconds." | Add-Content -LiteralPath $errorPath
        }
    }
    finally {
        $process.Dispose()
    }
    $status = switch ($testExitCode) {
        0       { 'passed' }
        77      { 'unavailable' }
        default { 'failed' }
    }
    [ordered]@{
        status = $status
        exit_code = $testExitCode
        executable = $TestExecutable
        worker_pid = $PID
    } | ConvertTo-Json | Set-Content -LiteralPath "$resultPath.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$resultPath.tmp" -Destination $resultPath
    exit $testExitCode
}

if (Test-Path -LiteralPath $resultPath) {
    throw "Use a fresh evidence directory; a result already exists: $resultPath"
}

# WMI creation leaves the invoking terminal's Job hierarchy. The test itself
# checks that premise and returns 77 if this host cannot supply the context.
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$command = '"{0}" -NoProfile -ExecutionPolicy Bypass -File "{1}" -Worker -TestExecutable "{2}" -EvidenceDirectory "{3}" -Scenario "{4}"' -f `
    $powershell, $PSCommandPath, $TestExecutable, $EvidenceDirectory, $Scenario
$created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $command }
if ($created.ReturnValue -ne 0) {
    throw "Win32_Process.Create failed: $($created.ReturnValue)"
}

$deadline = [DateTime]::UtcNow.AddSeconds($timeoutSeconds + 20)
while (-not (Test-Path -LiteralPath $resultPath)) {
    if ([DateTime]::UtcNow -ge $deadline) {
        throw "$Scenario check did not finish; worker PID $($created.ProcessId)."
    }
    Start-Sleep -Milliseconds 100
}
Get-Content -LiteralPath $logPath
Get-Content -LiteralPath (Join-Path $EvidenceDirectory "$Scenario.stderr.log")
$result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
Write-Host "$Scenario coverage: $($result.status) (exit $($result.exit_code))"
exit $result.exit_code
