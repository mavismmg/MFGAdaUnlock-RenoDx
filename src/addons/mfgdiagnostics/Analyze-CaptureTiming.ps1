# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromPipeline = $true)]
    [string[]]$Path
)

$ErrorActionPreference = 'Stop'

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    $ordered = @($Values | Sort-Object)
    $index = [Math]::Min($ordered.Count - 1,
        [Math]::Max(0, [int][Math]::Floor(($ordered.Count - 1) * $Percentile)))
    return [Math]::Round($ordered[$index], 3)
}

function Get-DurationSummary {
    param([object[]]$Events, [double]$Frequency)
    $values = @($Events | ForEach-Object {
        if ($null -ne $_.begin_qpc -and $null -ne $_.end_qpc) {
            [double]($_.end_qpc - $_.begin_qpc) * 1000000.0 / $Frequency
        }
    })
    [pscustomobject]@{
        samples = $values.Count
        p50_us = Get-Percentile $values 0.50
        p95_us = Get-Percentile $values 0.95
        p99_us = Get-Percentile $values 0.99
        max_us = if ($values.Count) { [Math]::Round(($values | Measure-Object -Maximum).Maximum, 3) } else { $null }
    }
}

function Get-NvapiSummary {
    param([object]$Nvapi)
    if ($null -eq $Nvapi) { return $null }

    $latencyFrames = @($Nvapi.latency_frames)
    $aiFrameTimes = @($latencyFrames | ForEach-Object {
        if ($null -ne $_.ai_frame_time_us -and [double]$_.ai_frame_time_us -gt 0) {
            [double]$_.ai_frame_time_us
        }
    })
    $gpuFrameTimes = @($latencyFrames | ForEach-Object {
        if ($null -ne $_.gpu_frame_time_us -and [double]$_.gpu_frame_time_us -gt 0) {
            [double]$_.gpu_frame_time_us
        }
    })

    [pscustomobject]@{
        library_loaded = $Nvapi.library_loaded
        initialize_status = $Nvapi.initialize_status
        sleep_status_result = $Nvapi.sleep_status_result
        latency_result = $Nvapi.latency_result
        ngx_override_result = $Nvapi.ngx_override_result
        sleep = $Nvapi.sleep
        valid_latency_frames = $latencyFrames.Count
        ai_frame_time_us = [pscustomobject]@{
            samples = $aiFrameTimes.Count
            p50 = Get-Percentile $aiFrameTimes 0.50
            p95 = Get-Percentile $aiFrameTimes 0.95
            p99 = Get-Percentile $aiFrameTimes 0.99
        }
        gpu_frame_time_us = [pscustomobject]@{
            samples = $gpuFrameTimes.Count
            p50 = Get-Percentile $gpuFrameTimes 0.50
            p95 = Get-Percentile $gpuFrameTimes 0.95
            p99 = Get-Percentile $gpuFrameTimes 0.99
        }
        ngx_override = $Nvapi.ngx_override
    }
}

foreach ($capturePath in $Path) {
    $capture = Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
    if ($capture.schema -ne 1 -or $null -eq $capture.events) {
        throw "Unsupported or incomplete capture: $capturePath"
    }

    $events = @($capture.events)
    $frequency = [double]$capture.qpc_frequency
    $constants = @($events | Where-Object { $_.kind -eq 1 -and $_.recognized })
    $orderedConstants = @($constants | Where-Object { $null -ne $_.frame } | Sort-Object begin_qpc)
    $outputs = @($events | Where-Object { $_.kind -eq 4 -and $_.recognized } | Sort-Object begin_qpc)

    $duplicateFrameCalls = 0
    $backwardFrameSteps = 0
    $skippedFrameIds = 0
    for ($index = 1; $index -lt $orderedConstants.Count; ++$index) {
        $delta = [int64]$orderedConstants[$index].frame - [int64]$orderedConstants[$index - 1].frame
        if ($delta -eq 0) { ++$duplicateFrameCalls }
        elseif ($delta -lt 0) { ++$backwardFrameSteps }
        elseif ($delta -gt 1) { $skippedFrameIds += $delta - 1 }
    }

    $outputIntervalsUs = @()
    for ($index = 1; $index -lt $outputs.Count; ++$index) {
        $outputIntervalsUs += [double]($outputs[$index].begin_qpc - $outputs[$index - 1].begin_qpc) *
            1000000.0 / $frequency
    }

    $jitterPairs = @($constants | ForEach-Object {
        if ($null -ne $_.floats[80] -and $null -ne $_.floats[81]) {
            '{0:R},{1:R}' -f [double]$_.floats[80], [double]$_.floats[81]
        }
    } | Sort-Object -Unique)

    $durations = [ordered]@{}
    $kindNames = @('options', 'constants', 'tag', 'state', 'output', 'tag_batch')
    for ($kind = 0; $kind -lt $kindNames.Count; ++$kind) {
        $kindEvents = @($events | Where-Object { $_.kind -eq $kind })
        $durations[$kindNames[$kind]] = Get-DurationSummary $kindEvents $frequency
    }

    $tagCounts = [ordered]@{}
    @($events | Where-Object { $_.kind -eq 2 -and $_.recognized } |
        Group-Object { $_.values[0] }) | ForEach-Object {
            $tagCounts[[string]$_.Name] = $_.Count
        }

    [pscustomobject]@{
        file = (Resolve-Path -LiteralPath $capturePath).Path
        label = $capture.label
        total_events = $events.Count
        dropped_events = $capture.dropped_events
        capacity_exhausted = $capture.capacity_exhausted
        constants = [pscustomobject]@{
            calls = $constants.Count
            unique_frame_ids = @($orderedConstants | Select-Object -ExpandProperty frame -Unique).Count
            duplicate_frame_steps = $duplicateFrameCalls
            backward_frame_steps = $backwardFrameSteps
            skipped_frame_ids = $skippedFrameIds
            reset_true = @($constants | Where-Object { $_.values[3] -eq 1 }).Count
            unique_jitter_pairs = $jitterPairs.Count
            conventions = @($constants | ForEach-Object {
                '{0}/{1}/{2}/{3}/{4}/{5}' -f $_.values[0],$_.values[1],$_.values[2],
                    $_.values[4],$_.values[5],$_.values[6]
            } | Sort-Object -Unique)
        }
        output = [pscustomobject]@{
            presents_observed = $outputs.Count
            interval_p50_us = Get-Percentile $outputIntervalsUs 0.50
            interval_p95_us = Get-Percentile $outputIntervalsUs 0.95
            interval_p99_us = Get-Percentile $outputIntervalsUs 0.99
        }
        options_calls = @($events | Where-Object { $_.kind -eq 0 }).Count
        state_calls = @($events | Where-Object { $_.kind -eq 3 }).Count
        tags_by_type_id = $tagCounts
        observed_call_durations = $durations
        nvapi_at_export = Get-NvapiSummary $capture.nvapi_at_export
        note = 'Durations include the downstream Streamline call and diagnostic overhead; output intervals are not display scanout intervals. NVAPI is sampled only when export is requested.'
    } | ConvertTo-Json -Depth 8
}
