# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [string]$Candidate
)
$ErrorActionPreference = 'Stop'

function Get-FormatName([object]$Format) {
    if ($null -eq $Format) { return 'unknown' }
    switch ([int]$Format) {
        10 { 'R16G16B16A16_FLOAT'; break }
        24 { 'R10G10B10A2_UNORM'; break }
        28 { 'R8G8B8A8_UNORM'; break }
        34 { 'R16G16_FLOAT'; break }
        40 { 'D32_FLOAT'; break }
        41 { 'R32_FLOAT'; break }
        45 { 'D24_UNORM_S8_UINT'; break }
        default { "format:$Format" }
    }
}

function Get-TagName([object]$Type) {
    if ($null -eq $Type) { return 'unknown' }
    switch ([int]$Type) {
        0 { 'Depth'; break }
        1 { 'MotionVectors'; break }
        2 { 'HUDLessColor'; break }
        13 { 'Exposure (not proof of FG consumption)'; break }
        23 { 'UIColorAndAlpha'; break }
        50 { 'BidirectionalDistortion'; break }
        53 { 'Backbuffer'; break }
        69 { 'UIAlpha'; break }
        default { "tag:$Type" }
    }
}

function Get-NvapiSummary([object]$Nvapi) {
    if ($null -eq $Nvapi) { return $null }
    $frames = @($Nvapi.latency_frames)
    [pscustomobject]@{
        LibraryLoaded = $Nvapi.library_loaded
        InitializeStatus = $Nvapi.initialize_status
        SleepStatus = $Nvapi.sleep_status_result
        LatencyStatus = $Nvapi.latency_result
        NgxOverrideStatus = $Nvapi.ngx_override_result
        DriverObservedFGMultiplier = $Nvapi.sleep.frame_generation_multiplier
        LowLatencyMode = $Nvapi.sleep.low_latency_mode
        GameSleep = $Nvapi.sleep.game_sleep
        IndependentFlip = $Nvapi.sleep.fullscreen_independent_flip
        FullscreenVRR = $Nvapi.sleep.fullscreen_vrr
        DynamicFG = $Nvapi.sleep.dynamic_frame_generation_control
        ValidLatencyFrames = $frames.Count
        AIFrameTimeSamples = @($frames | Where-Object { [double]$_.ai_frame_time_us -gt 0 }).Count
        NgxOverride = $Nvapi.ngx_override
    }
}

function Get-CaptureSummary([string]$CapturePath) {
    $capture = Get-Content -LiteralPath $CapturePath -Raw | ConvertFrom-Json
    if ($capture.schema -ne 1 -or $null -eq $capture.events) {
        throw "Unsupported or incomplete capture: $CapturePath"
    }
    $events = @($capture.events)
    $outputs = @($events | Where-Object { $_.kind -eq 4 })
    $options = @($events | Where-Object { $_.kind -eq 0 -and $_.recognized })
    $constants = @($events | Where-Object { $_.kind -eq 1 -and $_.recognized })
    $tags = @($events | Where-Object { $_.kind -eq 2 -and $_.recognized })
    $states = @($events | Where-Object { $_.kind -eq 3 -and $_.recognized })
    $colorSpaces = @{ 0 = 'unknown'; 1 = 'SDR/sRGB'; 2 = 'scRGB'; 3 = 'HDR10/PQ'; 4 = 'HDR/HLG' }
    $outputModes = @($outputs | ForEach-Object {
        '{0}; {1}; {2}x{3}; {4} buffers' -f $colorSpaces[[int]$_.values[1]],
            (Get-FormatName $_.values[2]), $_.values[3], $_.values[4], $_.values[5]
    } | Sort-Object -Unique)
    $tagModes = @($tags | ForEach-Object {
        $width = $_.values[5]; $height = $_.values[6]; $format = $_.values[7]
        if ($_.values[20] -eq 1) {
            $width = $_.values[14]; $height = $_.values[15]; $format = $_.values[16]
        }
        '{0}; resource={1}; native={2}; {3}x{4}; {5}; extent=({6},{7},{8},{9}); lifecycle={10}; state={11}' -f
            (Get-TagName $_.values[0]), $_.values[2], $_.values[3], $width, $height,
            (Get-FormatName $format), $_.values[9], $_.values[10], $_.values[11], $_.values[12],
            $_.values[1], $_.values[8]
    } | Sort-Object -Unique)
    $optionModes = @($options | ForEach-Object {
        'mode={0}; generated={1}; flags={2}; output={3}x{4}; color={5}; HUDless={6}; UI={7}; recomposition={8}' -f
            $_.values[0], $_.values[1], $_.values[2], $_.values[8], $_.values[9],
            (Get-FormatName $_.values[10]), (Get-FormatName $_.values[13]),
            (Get-FormatName $_.values[14]), $_.values[16]
    } | Sort-Object -Unique)
    $conventions = @($constants | ForEach-Object {
        'viewport={0}; MVscale=({1},{2}); invertedDepth={3}; cameraMotion={4}; 3D={5}; dilated={6}; jitteredMV={7}' -f
            $_.viewport, $_.floats[82], $_.floats[83], $_.values[0], $_.values[1],
            $_.values[2], $_.values[5], $_.values[6]
    } | Sort-Object -Unique)
    [pscustomobject]@{
        Label = $capture.label
        EventCount = $events.Count
        Dropped = $capture.dropped_events
        Full = $capture.capacity_exhausted
        TruncatedTagBatches = $capture.truncated_tag_batches
        UnrecognizedEvents = @($events | Where-Object { -not $_.recognized }).Count
        OptionsDownstream = $capture.options_downstream_owner
        OutputModes = $outputModes
        Options = $optionModes
        TagLayouts = $tagModes
        ConstantConventions = $conventions
        ResetTrueSamples = @($constants | Where-Object { $_.values[3] -eq 1 }).Count
        LegacyTagSamples = @($tags | Where-Object { $null -eq $_.frame }).Count
        HDRFormatFailureSamples = @($states | Where-Object { ([int]$_.values[0] -band 4) -ne 0 }).Count
        InvalidConstantsSamples = @($states | Where-Object { ([int]$_.values[0] -band 8) -ne 0 }).Count
        PresentationCounts = @($states | Group-Object { $_.values[1] } | ForEach-Object {
            [pscustomobject]@{ CountSinceApplicationQuery = $_.Name; Samples = $_.Count }
        })
        NvapiAtExport = Get-NvapiSummary $capture.nvapi_at_export
        ModulesAtExport = $capture.loaded_candidates_at_export
    }
}

$baselineSummary = Get-CaptureSummary $Baseline
$baselineSummary | ConvertTo-Json -Depth 12
if ($Candidate) {
    $candidateSummary = Get-CaptureSummary $Candidate
    $candidateSummary | ConvertTo-Json -Depth 12
    foreach ($field in @('OutputModes', 'Options', 'TagLayouts', 'ConstantConventions')) {
        $before = @($baselineSummary.$field)
        $after = @($candidateSummary.$field)
        # Compare-Object does not accept null/empty collections consistently in PS5.
        $removed = @($before | Where-Object { $_ -notin $after })
        $added = @($after | Where-Object { $_ -notin $before })
        [pscustomobject]@{ Field = $field; OnlyBaseline = $removed; OnlyCandidate = $added } |
            ConvertTo-Json -Depth 6
    }
}
Write-Output 'Interpretation: metadata differences are investigation targets, not proof of a defect. Presentation counts are per application query, not GPU frame intervals. Missing/truncated events cannot prove missing inputs. Compare image quality and final display pacing separately.'
