[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$Path,

  [string]$Label = ''
)

$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$style = [Globalization.NumberStyles]::Float

function Convert-Number([object]$Value) {
  if ($null -eq $Value) { return $null }
  $number = 0.0
  if ([double]::TryParse([string]$Value, $style, $culture, [ref]$number) -and
      -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)) {
    return $number
  }
  return $null
}

function Get-ColumnValues([object[]]$Rows, [string[]]$Names, [switch]$Positive) {
  $properties = @($Rows[0].PSObject.Properties.Name)
  $name = $Names | Where-Object { $properties -contains $_ } | Select-Object -First 1
  if ($null -eq $name) { return @() }
  $values = foreach ($row in $Rows) {
    $value = Convert-Number $row.$name
    if ($null -ne $value -and (-not $Positive -or $value -gt 0)) { $value }
  }
  return @($values)
}

function Get-Percentile([double[]]$Values, [double]$Fraction) {
  if ($Values.Count -eq 0) { return $null }
  $sorted = @($Values | Sort-Object)
  if ($sorted.Count -eq 1) { return $sorted[0] }
  $position = ($sorted.Count - 1) * $Fraction
  $lower = [math]::Floor($position)
  $upper = [math]::Ceiling($position)
  if ($lower -eq $upper) { return $sorted[$lower] }
  return $sorted[$lower] + ($sorted[$upper] - $sorted[$lower]) * ($position - $lower)
}

function Get-Mean([double[]]$Values) {
  if ($Values.Count -eq 0) { return $null }
  return ($Values | Measure-Object -Average).Average
}

function Get-SeriesSummary([double[]]$Values, [switch]$Rate) {
  if ($Values.Count -eq 0) { return $null }
  $median = Get-Percentile $Values 0.50
  $deviations = @($Values | ForEach-Object { [math]::Abs($_ - $median) })
  $mad = Get-Percentile $deviations 0.50
  $threshold = [math]::Max(2.0 * $median, $median + 3.0 * $mad)
  $mean = Get-Mean $Values
  $summary = [ordered]@{
    samples = $Values.Count
    mean_ms = [math]::Round($mean, 4)
    p50_ms = [math]::Round($median, 4)
    p95_ms = [math]::Round((Get-Percentile $Values 0.95), 4)
    p99_ms = [math]::Round((Get-Percentile $Values 0.99), 4)
    large_interval_threshold_ms = [math]::Round($threshold, 4)
    large_intervals = @($Values | Where-Object { $_ -gt $threshold }).Count
  }
  if ($Rate) {
    $summary.average_fps = if ($mean -gt 0) { [math]::Round(1000.0 / $mean, 2) } else { $null }
    $p99 = Get-Percentile $Values 0.99
    $summary.one_percent_low_fps = if ($p99 -gt 0) {
      [math]::Round(1000.0 / $p99, 2)
    } else { $null }
  }
  return $summary
}

$rows = @(Import-Csv -LiteralPath $Path)
if ($rows.Count -eq 0) { throw 'The PresentMon CSV has no rows.' }

$groups = @($rows | Group-Object SwapChainAddress)
$selected = $groups | Sort-Object Count -Descending | Select-Object -First 1
$swapchainRows = @($selected.Group)
$presentModeGroups = @($swapchainRows | Group-Object PresentMode)
$selectedPresentMode = $presentModeGroups | Sort-Object Count -Descending | Select-Object -First 1
$mainRows = if ($null -ne $selectedPresentMode) { @($selectedPresentMode.Group) } else { $swapchainRows }

# NVIDIA-generated frames are not always identified in FrameType. With current
# PresentMon v2 output they can still be distinguished conservatively when the
# driver emits AnimationTime=0 for generated samples and a positive animation
# timestamp for source application frames. Keep this explicitly heuristic.
$animationRows = @()
$sourceAnimationRows = @()
$generatedAnimationRows = @()
if ($mainRows[0].PSObject.Properties.Name -contains 'AnimationTime') {
  foreach ($row in $mainRows) {
    $animationTime = Convert-Number $row.AnimationTime
    if ($null -eq $animationTime) { continue }
    $animationRows += $row
    if ($animationTime -gt 0) { $sourceAnimationRows += $row }
    elseif ($animationTime -eq 0) { $generatedAnimationRows += $row }
  }
}
$useAnimationSplit = $sourceAnimationRows.Count -ge 30 -and
  $animationRows.Count -ge (1.5 * $sourceAnimationRows.Count)
$applicationRows = if ($useAnimationSplit) { $sourceAnimationRows } else { $mainRows }

# PresentMon 1.x-compatible output exposes MsBetweenDisplayChange. In v2 output,
# DisplayedTime is the closest display-duration metric. CapFrameX also recommends
# the former for NVIDIA MFG because it includes generated display flips.
$display = Get-ColumnValues $mainRows @('MsBetweenDisplayChange', 'DisplayedTime') -Positive
$application = Get-ColumnValues $applicationRows @('MsBetweenSimulationStart', 'FrameTime', 'MsBetweenPresents') -Positive
$present = Get-ColumnValues $mainRows @('MsBetweenPresents') -Positive
$presentApi = Get-ColumnValues $mainRows @('MsInPresentAPI')
$gpuBusy = Get-ColumnValues $mainRows @('GPUBusy', 'MsGPUBusy') -Positive
$gpuTime = Get-ColumnValues $mainRows @('GPUTime', 'MsGPUTime') -Positive
$sourceGpuBusy = Get-ColumnValues $applicationRows @('GPUBusy', 'MsGPUBusy') -Positive
$sourceGpuTime = Get-ColumnValues $applicationRows @('GPUTime', 'MsGPUTime') -Positive
$generatedGpuBusy = Get-ColumnValues $generatedAnimationRows @('GPUBusy', 'MsGPUBusy') -Positive
$generatedGpuTime = Get-ColumnValues $generatedAnimationRows @('GPUTime', 'MsGPUTime') -Positive
$pcLatency = Get-ColumnValues $mainRows @('MsPCLatency', 'InstrumentedLatency', 'DisplayLatency') -Positive
$sourceLatency = Get-ColumnValues $applicationRows @('MsPCLatency', 'InstrumentedLatency', 'DisplayLatency') -Positive
$generatedLatency = Get-ColumnValues $generatedAnimationRows @('MsPCLatency', 'InstrumentedLatency', 'DisplayLatency') -Positive
$animationError = Get-ColumnValues $mainRows @('MsAnimationError', 'AnimationError')

$displaySummary = Get-SeriesSummary $display -Rate
$applicationSummary = Get-SeriesSummary $application -Rate
$displayDurationMs = if ($display.Count) { ($display | Measure-Object -Sum).Sum } else { 0.0 }
$inferredGeneration = $null
if ($useAnimationSplit) {
  $rawRatio = [double]$animationRows.Count / [double]$sourceAnimationRows.Count
  $inferredGeneration = [ordered]@{
    method = 'AnimationTime zero/positive split (heuristic)'
    source_application_frames = $sourceAnimationRows.Count
    generated_frame_candidates = $generatedAnimationRows.Count
    total_frame_ratio = [math]::Round($rawRatio, 3)
    nearest_total_multiplier = [math]::Max(1, [int][math]::Round($rawRatio))
    estimated_source_fps = if ($displayDurationMs -gt 0) {
      [math]::Round(1000.0 * $sourceAnimationRows.Count / $displayDurationMs, 2)
    } else { $null }
    estimated_displayed_fps = if ($displayDurationMs -gt 0) {
      [math]::Round(1000.0 * $display.Count / $displayDurationMs, 2)
    } else { $null }
  }
}
$frameTypes = [ordered]@{}
if ($mainRows[0].PSObject.Properties.Name -contains 'FrameType') {
  foreach ($group in @($mainRows | Group-Object FrameType | Sort-Object Name)) {
    $frameTypes[[string]$group.Name] = $group.Count
  }
}

$report = [ordered]@{
  schema = 1
  label = $Label
  source = (Resolve-Path -LiteralPath $Path).Path
  process = $mainRows[0].Application
  process_id = $mainRows[0].ProcessID
  selected_swapchain = $selected.Name
  selected_swapchain_rows = $swapchainRows.Count
  selected_present_mode = if ($null -ne $selectedPresentMode) { $selectedPresentMode.Name } else { $null }
  selected_rows = $mainRows.Count
  other_swapchains = [math]::Max(0, $groups.Count - 1)
  display_intervals = $displaySummary
  application_intervals = $applicationSummary
  present_intervals = Get-SeriesSummary $present -Rate
  present_api_ms = Get-SeriesSummary $presentApi
  gpu_busy_ms = Get-SeriesSummary $gpuBusy
  gpu_time_ms = Get-SeriesSummary $gpuTime
  source_frame_gpu_busy_ms = Get-SeriesSummary $sourceGpuBusy
  source_frame_gpu_time_ms = Get-SeriesSummary $sourceGpuTime
  generated_candidate_gpu_busy_ms = Get-SeriesSummary $generatedGpuBusy
  generated_candidate_gpu_time_ms = Get-SeriesSummary $generatedGpuTime
  pc_or_instrumented_latency_ms = Get-SeriesSummary $pcLatency
  source_frame_latency_ms = Get-SeriesSummary $sourceLatency
  generated_candidate_latency_ms = Get-SeriesSummary $generatedLatency
  animation_error_ms = Get-SeriesSummary $animationError
  frame_types = $frameTypes
  inferred_frame_generation = $inferredGeneration
  estimated_display_to_application_ratio = if ($null -ne $displaySummary -and
      $null -ne $applicationSummary -and $applicationSummary.average_fps -gt 0) {
    [math]::Round($displaySummary.average_fps / $applicationSummary.average_fps, 3)
  } else { $null }
  cautions = @(
    'Compare only repeated runs of the same save, route, settings, driver and DLL versions.',
    'FrameType may remain Application/Unknown for NVIDIA-generated frames; use display timing and NVAPI telemetry as corroboration.',
    'The AnimationTime split is driver-dependent inference, not an authoritative NVIDIA frame label.',
    'Rows from presentation-mode transitions are excluded by selecting the dominant PresentMode.',
    'HAGS can make PresentMon GPU execution timing less accurate; use Nsight GPU Trace for kernel conclusions.',
    'A large-interval count is a robust outlier indicator, not a universal definition of perceptible stutter.'
  )
}

$jsonPath = [IO.Path]::ChangeExtension((Resolve-Path -LiteralPath $Path).Path, '.summary.json')
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$report | ConvertTo-Json -Depth 8
Write-Host "Summary saved to: $jsonPath"
