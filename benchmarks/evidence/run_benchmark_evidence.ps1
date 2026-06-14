param(
    [ValidateSet("smoke", "evidence")]
    [string] $Mode = "smoke",
    [string] $InputEchoExe = "",
    [string] $SurfaceStressExe = "",
    [string] $ArtifactDir = "",
    [string] $OutputJson = "",
    [string] $BuildDir = "",
    [string] $SurfaceRepo = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string] $AppRepo = "",
    [string] $ChromeRepo = "",
    [int] $RepeatCount = 0,
    [int] $DirtyRowSeed = 37,
    [switch] $SkipInputEcho,
    [switch] $SkipSurfaceStress
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($ArtifactDir -eq "") {
    $ArtifactDir = Join-Path $SurfaceRepo "artifacts\benchmark_evidence_${Mode}"
}
if ($OutputJson -eq "") {
    $OutputJson = Join-Path $ArtifactDir "benchmark_evidence_${Mode}.json"
}
if ($RepeatCount -le 0) {
    if ($Mode -eq "smoke") {
        $RepeatCount = 1
    }
    else {
        $RepeatCount = 10
    }
}

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$failures = New-Object System.Collections.Generic.List[string]
$commands = New-Object System.Collections.Generic.List[object]

function Add-Failure {
    param([string] $Message)

    $script:failures.Add($Message)
}

function Test-ExecutablePath {
    param(
        [string] $Path,
        [string] $Name
    )

    if ($Path -eq "" -or !(Test-Path -LiteralPath $Path)) {
        Add-Failure "$Name executable is missing: $Path"
        return $false
    }

    return $true
}

function Join-CommandLine {
    param(
        [string]   $FilePath,
        [string[]] $Arguments
    )

    $tokens = @($FilePath) + $Arguments
    return ($tokens | ForEach-Object {
        '"' + ($_ -replace '"', '\"') + '"'
    }) -join " "
}

function Invoke-CapturedCommand {
    param(
        [string]   $Id,
        [string]   $FilePath,
        [string[]] $Arguments,
        [string]   $WorkingDirectory,
        [string]   $StdoutPath
    )

    New-Item -ItemType Directory -Force -Path (Split-Path $StdoutPath -Parent) | Out-Null
    $commandText = Join-CommandLine $FilePath $Arguments
    @(
        "working_directory=$WorkingDirectory",
        "command=$commandText",
        ""
    ) | Set-Content -Encoding ASCII -Path $StdoutPath

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $WorkingDirectory
    try {
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        foreach ($line in $output) {
            $text = $line.ToString()
            Add-Content -Encoding ASCII -Path $StdoutPath -Value $text
            Write-Host $text
        }
        $ErrorActionPreference = $previousErrorActionPreference
    }
    finally {
        if (Get-Variable -Name previousErrorActionPreference -Scope Local -ErrorAction SilentlyContinue) {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        Pop-Location
        $timer.Stop()
    }

    $record = [ordered]@{
        id = $Id
        command_line = $commandText
        working_directory = $WorkingDirectory
        stdout_path = $StdoutPath
        exit_code = $exitCode
        elapsed_ms = [Math]::Round($timer.Elapsed.TotalMilliseconds, 3)
    }
    $script:commands.Add($record)

    if ($exitCode -ne 0) {
        Add-Failure "Command failed with exit code ${exitCode}: $Id"
    }

    return $record
}

function Get-RepoMetadata {
    param(
        [string] $Name,
        [string] $Path
    )

    if ($Path -eq "" -or !(Test-Path -LiteralPath $Path)) {
        return [ordered]@{
            name = $Name
            path = $Path
            available = $false
        }
    }

    $head = (& git -C $Path rev-parse HEAD 2>$null)
    $branch = (& git -C $Path branch --show-current 2>$null)
    $status = @(& git -C $Path status --short 2>$null)
    return [ordered]@{
        name = $Name
        path = (Resolve-Path $Path).Path
        available = $true
        head = $head
        branch = $branch
        dirty = ($status.Count -gt 0)
        status_short = $status
    }
}

function Get-CMakeCacheValue {
    param(
        [hashtable] $Cache,
        [string]    $Name
    )

    if ($Cache.ContainsKey($Name)) {
        return $Cache[$Name]
    }
    return $null
}

function Get-BuildMetadata {
    param([string] $Path)

    if ($Path -eq "" -or !(Test-Path -LiteralPath $Path)) {
        return [ordered]@{
            available = $false
            path = $Path
        }
    }

    $cachePath = Join-Path $Path "CMakeCache.txt"
    if (!(Test-Path -LiteralPath $cachePath)) {
        return [ordered]@{
            available = $false
            path = (Resolve-Path $Path).Path
            cmake_cache = $cachePath
        }
    }

    $cache = @{}
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match "^(?<key>[^#/][^:=]*)(:[^=]*)?=(?<value>.*)$") {
            $cache[$Matches["key"]] = $Matches["value"]
        }
    }

    return [ordered]@{
        available = $true
        path = (Resolve-Path $Path).Path
        cmake_cache = $cachePath
        source_dir = Get-CMakeCacheValue $cache "CMAKE_HOME_DIRECTORY"
        generator = Get-CMakeCacheValue $cache "CMAKE_GENERATOR"
        build_type = Get-CMakeCacheValue $cache "CMAKE_BUILD_TYPE"
        qt6_dir = Get-CMakeCacheValue $cache "Qt6_DIR"
        build_benchmarks = Get-CMakeCacheValue $cache "VNM_TERMINAL_BUILD_BENCHMARKS"
        profiling = Get-CMakeCacheValue $cache "VNM_TERMINAL_ENABLE_PROFILING"
    }
}

function Get-MachineMetadata {
    $os = $null
    $cpu = $null
    $gpu = @()
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
            }
        })
    }
    catch {
    }

    return [ordered]@{
        computer_name = $env:COMPUTERNAME
        user_name = $env:USERNAME
        os_caption = if ($os -ne $null) { $os.Caption } else { $null }
        os_version = if ($os -ne $null) { $os.Version } else { $null }
        cpu_name = if ($cpu -ne $null) { $cpu.Name } else { $null }
        logical_processor_count = if ($cpu -ne $null) { $cpu.NumberOfLogicalProcessors } else { $null }
        gpu = $gpu
        powershell = $PSVersionTable.PSVersion.ToString()
        process_architecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    }
}

function Convert-MetricValue {
    param([string] $Value)

    if ($Value -eq "true") {
        return $true
    }
    if ($Value -eq "false") {
        return $false
    }

    $number = 0.0
    if ([double]::TryParse(
            $Value,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref] $number))
    {
        return $number
    }

    return $Value
}

function Read-KeyValueMetrics {
    param([string] $Path)

    $metrics = [ordered]@{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) {
            continue
        }

        $key = $line.Substring(0, $separator).Trim()
        $value = $line.Substring($separator + 1).Trim()
        if ($key -ne "") {
            $metrics[$key] = Convert-MetricValue $value
        }
    }
    return $metrics
}

function Get-Percentile {
    param(
        [double[]] $SortedValues,
        [double]   $Percentile
    )

    if ($SortedValues.Count -eq 0) {
        return $null
    }

    $index = [Math]::Ceiling(($Percentile / 100.0) * $SortedValues.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($SortedValues.Count - 1, $index))
    return $SortedValues[$index]
}

function Get-NumericSummary {
    param(
        [object[]] $Records,
        [string]   $MetricName
    )

    $values = @()
    foreach ($record in $Records) {
        if ($record.Contains($MetricName) -and $record[$MetricName] -is [ValueType]) {
            $values += [double] $record[$MetricName]
        }
    }

    if ($values.Count -eq 0) {
        return [ordered]@{
            sample_count = 0
        }
    }

    [double[]] $sorted = @($values | Sort-Object)
    $median = Get-Percentile $sorted 50
    $p95 = Get-Percentile $sorted 95
    $deviations = @($values | ForEach-Object { [Math]::Abs($_ - $median) })
    [double[]] $sortedDeviations = @($deviations | Sort-Object)
    $mad = Get-Percentile $sortedDeviations 50
    $outliers = @()
    for ($index = 0; $index -lt $values.Count; ++$index) {
        $distance = [Math]::Abs($values[$index] - $median)
        if (($mad -gt 0.0 -and $distance -gt 3.0 * $mad) -or
            ($mad -eq 0.0 -and $distance -gt 0.0))
        {
            $outliers += [ordered]@{
                sample_index = $index
                value = $values[$index]
                distance_from_median = $distance
            }
        }
    }

    return [ordered]@{
        sample_count = $values.Count
        min = $sorted[0]
        median = $median
        p95 = $p95
        max = $sorted[$sorted.Count - 1]
        mean = ($values | Measure-Object -Average).Average
        median_absolute_deviation = $mad
        outlier_rule = "abs(value - median) > 3 * median_absolute_deviation; if MAD is zero, report non-median values"
        outliers = $outliers
    }
}

function Get-InputEchoConfig {
    if ($Mode -eq "smoke") {
        return [ordered]@{
            iterations = 4
            warmup = 1
            polish_delay_us = 2000
            pre_echo_backlog = 2
            catchup_budgets_us = "0,500,2000"
            echo_delay_us = "0,1000,4000"
        }
    }

    return [ordered]@{
        iterations = 30
        warmup = 5
        polish_delay_us = 2000
        pre_echo_backlog = 2
        catchup_budgets_us = "0,250,1000,4000"
        echo_delay_us = "0,500,1500,3000,6000"
    }
}

function Add-InputEchoCaseRecords {
    param(
        [object] $Root,
        [int]    $RepeatIndex,
        [string] $RawJsonPath,
        [System.Collections.Generic.List[object]] $Records
    )

    foreach ($case in $Root.cases) {
        $Records.Add([ordered]@{
            repeat_index = $RepeatIndex
            raw_json_path = $RawJsonPath
            catchup_budget_us = [int] $case.catchup_budget_us
            echo_delay_us = [int] $case.echo_delay_us
            sample_count = [int] $case.sample_count
            echo_eligible_for_polish_count = [int] $case.echo_eligible_for_polish_count
            echo_visible_at_polish_count = [int] $case.echo_visible_at_polish_count
            stale_frame_at_polish_count = [int] $case.stale_frame_at_polish_count
            cursor_stale_at_polish_count = [int] $case.cursor_stale_at_polish_count
            callbacks_queued_after_polish_count = [int] $case.callbacks_queued_after_polish_count
            max_cursor_stale_cells = [int] $case.max_cursor_stale_cells
            catchup_elapsed_ns_median = [double] $case.catchup_elapsed_ns.median
            catchup_elapsed_ns_p95 = [double] $case.catchup_elapsed_ns.p95
            input_to_polish_done_ns_median = [double] $case.input_to_polish_done_ns.median
            polish_elapsed_ns_median = [double] $case.polish_elapsed_ns.median
            budgeted_drain_calls = [double] $case.backend_drain_delta_total.budgeted_drain_calls
            budget_exhausted_incomplete = [double] $case.backend_drain_delta_total.budget_exhausted_incomplete
            pending_callback_after_drain = [double] $case.backend_drain_delta_total.pending_callback_after_drain
        })
    }
}

function Invoke-InputEchoEvidence {
    $config = Get-InputEchoConfig
    $records = New-Object System.Collections.Generic.List[object]
    $queueContractRecords = New-Object System.Collections.Generic.List[object]
    $rawFiles = New-Object System.Collections.Generic.List[string]
    $queueContractRawFiles = New-Object System.Collections.Generic.List[string]

    for ($repeat = 1; $repeat -le $RepeatCount; ++$repeat) {
        $repeatDir = Join-Path $ArtifactDir "input_echo\repeat_$repeat"
        New-Item -ItemType Directory -Force -Path $repeatDir | Out-Null
        $rawJson = Join-Path $repeatDir "input_echo_catchup.json"
        $stdout = Join-Path $repeatDir "input_echo_catchup_stdout.txt"
        $args = @(
            "--iterations", [string] $config.iterations,
            "--warmup", [string] $config.warmup,
            "--catchup-budget-us", [string] $config.catchup_budgets_us,
            "--echo-delay-us", [string] $config.echo_delay_us,
            "--polish-delay-us", [string] $config.polish_delay_us,
            "--pre-echo-backlog", [string] $config.pre_echo_backlog,
            "--output", $rawJson,
            "--quiet",
            "--validate-json"
        )

        Invoke-CapturedCommand `
            -Id "input_echo_catchup_repeat_$repeat" `
            -FilePath $InputEchoExe `
            -Arguments $args `
            -WorkingDirectory $SurfaceRepo `
            -StdoutPath $stdout | Out-Null

        if (!(Test-Path -LiteralPath $rawJson)) {
            Add-Failure "Input echo raw JSON missing for repeat $repeat"
            continue
        }

        $rawFiles.Add($rawJson)
        $root = Get-Content -Raw -LiteralPath $rawJson | ConvertFrom-Json
        if ($root.schema -ne "vnm_terminal_input_echo_catchup_benchmark" -or
            [int] $root.schema_version -ne 1)
        {
            Add-Failure "Input echo raw JSON schema mismatch in repeat $repeat"
            continue
        }

        Add-InputEchoCaseRecords `
            -Root $root `
            -RepeatIndex $repeat `
            -RawJsonPath $rawJson `
            -Records $records

        $queueRawJson = Join-Path $repeatDir "input_echo_queue_contract.json"
        $queueStdout = Join-Path $repeatDir "input_echo_queue_contract_stdout.txt"
        $queueArgs = @(
            "--queue-contract",
            "--output", $queueRawJson,
            "--quiet",
            "--validate-json"
        )

        Invoke-CapturedCommand `
            -Id "input_echo_queue_contract_repeat_$repeat" `
            -FilePath $InputEchoExe `
            -Arguments $queueArgs `
            -WorkingDirectory $SurfaceRepo `
            -StdoutPath $queueStdout | Out-Null

        if (!(Test-Path -LiteralPath $queueRawJson)) {
            Add-Failure "Input echo queue-contract JSON missing for repeat $repeat"
            continue
        }

        $queueContractRawFiles.Add($queueRawJson)
        $queueRoot = Get-Content -Raw -LiteralPath $queueRawJson | ConvertFrom-Json
        if ($queueRoot.schema -ne "vnm_terminal_input_echo_queue_contract_benchmark" -or
            [int] $queueRoot.schema_version -ne 1)
        {
            Add-Failure "Input echo queue-contract schema mismatch in repeat $repeat"
            continue
        }

        $contract = $queueRoot.queue_contract
        $queueContractRecords.Add([ordered]@{
            repeat_index = $repeat
            raw_json_path = $queueRawJson
            passed = [bool] $contract.passed
            stale_capture_observed = [bool] $contract.stale_capture_observed
            input_accepted = [bool] $contract.input_accepted
            echo_injected_before_sync = [bool] $contract.echo_injected_before_sync
            callback_enqueue_epoch_before_echo =
                [uint64] $contract.callback_enqueue_epoch_before_echo
            callback_enqueue_epoch_after_echo =
                [uint64] $contract.callback_enqueue_epoch_after_echo
            callback_processed_epoch_after_capture =
                [uint64] $contract.callback_processed_epoch_after_capture
            echo_snapshot_callback_epoch =
                [uint64] $contract.echo_snapshot_callback_epoch
            backend_callback_frame_deferrals_before =
                [uint64] $contract.backend_callback_frame_deferrals_before
            backend_callback_frame_deferrals_after =
                [uint64] $contract.backend_callback_frame_deferrals_after
        })
    }

    $expectedBudgets = @(([string] $config.catchup_budgets_us).Split(",") | ForEach-Object { [int] $_ })
    $expectedDelays = @(([string] $config.echo_delay_us).Split(",") | ForEach-Object { [int] $_ })
    $expectedCaseCount = $expectedBudgets.Count * $expectedDelays.Count * $RepeatCount
    if ($records.Count -ne $expectedCaseCount) {
        Add-Failure "Input echo case coverage mismatch: expected $expectedCaseCount, got $($records.Count)"
    }

    $abPairsPresent = $true
    foreach ($delay in $expectedDelays) {
        $zeroCases = @($records | Where-Object {
            $_["echo_delay_us"] -eq $delay -and $_["catchup_budget_us"] -eq 0
        })
        $nonzeroCases = @($records | Where-Object {
            $_["echo_delay_us"] -eq $delay -and $_["catchup_budget_us"] -gt 0
        })
        if ($zeroCases.Count -eq 0 -or $nonzeroCases.Count -eq 0) {
            $abPairsPresent = $false
        }
    }
    if (!$abPairsPresent) {
        Add-Failure "Input echo evidence lacks zero-budget versus nonzero-budget A/B pairs"
    }

    foreach ($record in $records) {
        if ($record["sample_count"] -ne $config.iterations) {
            Add-Failure "Input echo sample count mismatch in $($record["raw_json_path"])"
        }
        if ($record["echo_eligible_for_polish_count"] -gt 0 -and
            ($record["stale_frame_at_polish_count"] -gt 0 -or
                $record["cursor_stale_at_polish_count"] -gt 0))
        {
            Add-Failure "Input echo eligible case captured stale frame/cursor state"
        }
        if ($record["catchup_budget_us"] -eq 0 -and
            $record["echo_eligible_for_polish_count"] -gt 0 -and
            $record["echo_visible_at_polish_count"] -ne
                $record["echo_eligible_for_polish_count"])
        {
            Add-Failure "Input echo zero-budget eligible case did not publish fresh echo"
        }
    }

    foreach ($record in $queueContractRecords) {
        if (!$record["passed"] -or $record["stale_capture_observed"]) {
            Add-Failure "Input echo queue contract failed in $($record["raw_json_path"])"
        }
        if ($record["callback_enqueue_epoch_after_echo"] -le
            $record["callback_enqueue_epoch_before_echo"])
        {
            Add-Failure "Input echo queue contract did not advance enqueue epoch"
        }
        if ($record["echo_snapshot_callback_epoch"] -lt
            $record["callback_enqueue_epoch_after_echo"])
        {
            Add-Failure "Input echo queue contract echo snapshot epoch is stale"
        }
    }

    $metrics = @(
        "stale_frame_at_polish_count",
        "cursor_stale_at_polish_count",
        "callbacks_queued_after_polish_count",
        "catchup_elapsed_ns_median",
        "catchup_elapsed_ns_p95",
        "input_to_polish_done_ns_median",
        "polish_elapsed_ns_median",
        "budget_exhausted_incomplete",
        "pending_callback_after_drain"
    )
    $summaries = [ordered]@{}
    foreach ($metric in $metrics) {
        $summaries[$metric] = Get-NumericSummary `
            -Records @($records.ToArray()) `
            -MetricName $metric
    }

    return [ordered]@{
        schema = "vnm_terminal_input_echo_catchup_evidence_summary"
        schema_version = 1
        raw_files = $rawFiles
        queue_contract_raw_files = $queueContractRawFiles
        repeat_count = $RepeatCount
        config = $config
        decision_checks = [ordered]@{
            structural_only = $true
            performance_thresholds_encoded = $false
            zero_budget_baseline_present = ($expectedBudgets -contains 0)
            same_delay_ab_pairs_present = $abPairsPresent
            queue_contract_present = ($queueContractRecords.Count -eq $RepeatCount)
            pass_rule = "commands pass, raw schema validates, all cases are present, eligible zero-budget cases are fresh, and queue-contract captures are not stale"
            rejects_waited_long_enough_evidence = "queue-contract evidence proves callbacks already enqueued before sync are included without waiting for future output"
        }
        metric_summaries = $summaries
        case_records = $records
        queue_contract_records = $queueContractRecords
    }
}

function New-SurfaceCase {
    param(
        [string] $Category,
        [int]    $Rows,
        [int]    $Columns,
        [int]    $DirtyRows,
        [int]    $Stride,
        [string] $Pattern
    )

    return [ordered]@{
        category = $Category
        rows = $Rows
        columns = $Columns
        dirty_rows = $DirtyRows
        dirty_row_stride = $Stride
        text_pattern = $Pattern
    }
}

function Get-SurfaceMatrix {
    if ($Mode -eq "smoke") {
        return @(
            (New-SurfaceCase "smoke_sparse_ascii" 24 80 4 7 "ascii"),
            (New-SurfaceCase "smoke_sparse_block" 24 80 4 7 "block")
        )
    }

    $matrix = @()
    foreach ($pattern in @("ascii", "block")) {
        foreach ($dirtyRows in @(1, 8, 32, 235)) {
            foreach ($stride in @(1, 7)) {
                if ($dirtyRows -eq 235 -and $stride -ne 1) {
                    continue
                }
                $matrix += New-SurfaceCase `
                    "dirty_row_scale" `
                    235 `
                    873 `
                    $dirtyRows `
                    $stride `
                    $pattern
            }
        }
        foreach ($columns in @(160, 320, 873)) {
            $matrix += New-SurfaceCase `
                "column_scale" `
                235 `
                $columns `
                8 `
                7 `
                $pattern
        }
    }
    return $matrix
}

function Invoke-SurfaceStressEvidence {
    $records = New-Object System.Collections.Generic.List[object]
    $matrix = Get-SurfaceMatrix
    $frames = if ($Mode -eq "smoke") { 5 } else { 180 }
    $warmupFrames = if ($Mode -eq "smoke") { 2 } else { 10 }

    foreach ($case in $matrix) {
        for ($repeat = 1; $repeat -le $RepeatCount; ++$repeat) {
            $caseId = "$($case.category)_$($case.text_pattern)_r$($case.rows)_c$($case.columns)_d$($case.dirty_rows)_s$($case.dirty_row_stride)"
            $repeatDir = Join-Path $ArtifactDir "surface_stress\$caseId\repeat_$repeat"
            New-Item -ItemType Directory -Force -Path $repeatDir | Out-Null
            $stdout = Join-Path $repeatDir "surface_stress_stdout.txt"
            $args = @(
                "--frames", [string] $frames,
                "--warmup-frames", [string] $warmupFrames,
                "--rows", [string] $case.rows,
                "--cols", [string] $case.columns,
                "--dirty-rows", [string] $case.dirty_rows,
                "--dirty-row-stride", [string] $case.dirty_row_stride,
                "--dirty-row-seed", [string] $DirtyRowSeed,
                "--text-pattern", [string] $case.text_pattern,
                "--graphics-every", "0",
                "--style-period", "8"
            )

            Invoke-CapturedCommand `
                -Id "surface_stress_${caseId}_repeat_$repeat" `
                -FilePath $SurfaceStressExe `
                -Arguments $args `
                -WorkingDirectory $SurfaceRepo `
                -StdoutPath $stdout | Out-Null

            $metrics = Read-KeyValueMetrics $stdout
            foreach ($required in @(
                    "scenario",
                    "frames",
                    "warmup_frames",
                    "rows",
                    "columns",
                    "dirty_rows_requested",
                    "dirty_row_seed",
                    "text_pattern",
                    "snapshot_ms_per_frame",
                    "render_frame_ms_per_frame",
                    "snapshot_cells_per_frame",
                    "snapshot_dirty_rows_visible_per_frame",
                    "checksum"))
            {
                if (!$metrics.Contains($required)) {
                    Add-Failure "Surface stress output missing '$required' for $caseId repeat $repeat"
                }
            }

            $records.Add([ordered]@{
                repeat_index = $repeat
                case_id = $caseId
                category = $case.category
                raw_stdout_path = $stdout
                rows = [int] $case.rows
                columns = [int] $case.columns
                dirty_rows = [int] $case.dirty_rows
                dirty_row_stride = [int] $case.dirty_row_stride
                dirty_row_seed = $DirtyRowSeed
                text_pattern = [string] $case.text_pattern
                total_ms = [double] $metrics["total_ms"]
                frames_per_second = [double] $metrics["frames_per_second"]
                ingest_ms_per_frame = [double] $metrics["ingest_ms_per_frame"]
                snapshot_ms_per_frame = [double] $metrics["snapshot_ms_per_frame"]
                render_frame_ms_per_frame = [double] $metrics["render_frame_ms_per_frame"]
                snapshot_cells_per_frame = [double] $metrics["snapshot_cells_per_frame"]
                snapshot_dirty_rows_visible_per_frame = [double] $metrics["snapshot_dirty_rows_visible_per_frame"]
                frame_dirty_rows = [double] $metrics["frame_dirty_rows"]
                frame_full_dirty_rows = [double] $metrics["frame_full_dirty_rows"]
                frame_cells_considered = [double] $metrics["frame_cells_considered"]
                checksum = [double] $metrics["checksum"]
            })
        }
    }

    $expectedRecordCount = $matrix.Count * $RepeatCount
    if ($records.Count -ne $expectedRecordCount) {
        Add-Failure "Surface stress case coverage mismatch: expected $expectedRecordCount, got $($records.Count)"
    }

    $metricsToSummarize = @(
        "total_ms",
        "frames_per_second",
        "ingest_ms_per_frame",
        "snapshot_ms_per_frame",
        "render_frame_ms_per_frame",
        "snapshot_cells_per_frame",
        "snapshot_dirty_rows_visible_per_frame",
        "frame_cells_considered"
    )
    $summaries = [ordered]@{}
    foreach ($metric in $metricsToSummarize) {
        $summaries[$metric] = Get-NumericSummary `
            -Records @($records.ToArray()) `
            -MetricName $metric
    }

    return [ordered]@{
        schema = "vnm_terminal_lazy_snapshot_stage_evidence_summary"
        schema_version = 1
        repeat_count = $RepeatCount
        frames = $frames
        warmup_frames = $warmupFrames
        deterministic_seed = $DirtyRowSeed
        workload_matrix = $matrix
        decision_checks = [ordered]@{
            structural_only = $true
            performance_thresholds_encoded = $false
            production_enablement_status = "rejected"
            final_lazy_state = "evidence-only"
            pass_rule = "commands pass, required metrics are present, dirty-row validation in the executable passes, and every matrix case runs all repeats"
            deletion_or_rejection_rule = "stage-only surface stress evidence is retained only as evidence; production lazy publication remains rejected"
        }
        metric_summaries = $summaries
        case_records = $records
    }
}

$inputEcho = $null
$surfaceStress = $null

if (!$SkipInputEcho) {
    if (Test-ExecutablePath $InputEchoExe "Input echo catchup benchmark") {
        $inputEcho = Invoke-InputEchoEvidence
    }
}

if (!$SkipSurfaceStress) {
    if (Test-ExecutablePath $SurfaceStressExe "Surface stress benchmark") {
        $surfaceStress = Invoke-SurfaceStressEvidence
    }
}

$root = [ordered]@{
    schema = "vnm_terminal_benchmark_evidence_run"
    schema_version = 2
    generated_utc = (Get-Date).ToUniversalTime().ToString("o")
    mode = $Mode
    stability_contract = [ordered]@{
        warmup_policy = "underlying benchmark warmups are discarded before measured samples"
        repeat_policy = "each repeat is a separate process invocation"
        outlier_policy = "outliers are reported with MAD; no sample is removed by the runner"
        pass_fail_policy = "structural execution, schema, and matrix coverage only; no premature performance threshold"
        smoke_mode = "small case set for CI/local validation"
        evidence_mode = "representative repeated matrix for decision artifacts"
    }
    metadata = [ordered]@{
        machine = Get-MachineMetadata
        build = Get-BuildMetadata $BuildDir
        repositories = @(
            (Get-RepoMetadata "surface" $SurfaceRepo),
            (Get-RepoMetadata "app" $AppRepo),
            (Get-RepoMetadata "qml_chrome" $ChromeRepo)
        )
        environment = [ordered]@{
            QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM
            QSG_RENDER_LOOP = $env:QSG_RENDER_LOOP
            QSG_RHI_BACKEND = $env:QSG_RHI_BACKEND
            QT_SCALE_FACTOR = $env:QT_SCALE_FACTOR
            QT_SCALE_FACTOR_ROUNDING_POLICY = $env:QT_SCALE_FACTOR_ROUNDING_POLICY
        }
    }
    input_echo = $inputEcho
    lazy_snapshot_stage = $surfaceStress
    final_lazy_snapshot_state = [ordered]@{
        state = "evidence-only"
        production_enablement_status = "rejected"
        production_publication_path = "full_snapshot_only"
        lazy_composer_reachability = "internal_testing_and_benchmark_evidence_api"
        structural_only = $true
        performance_thresholds_encoded = $false
        default_production_enabled = $false
        no_production_switch = $true
    }
    commands = $commands
    result = [ordered]@{
        status = if ($failures.Count -eq 0) { "pass" } else { "fail" }
        failures = $failures
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path $OutputJson -Parent) | Out-Null
$root |
    ConvertTo-Json -Depth 32 |
    Set-Content -Encoding ASCII -Path $OutputJson

Write-Host "benchmark evidence ${Mode} output: $OutputJson"

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        [Console]::Error.WriteLine($failure)
    }
    exit 1
}

exit 0
