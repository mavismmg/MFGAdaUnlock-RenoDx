[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('native-no-addon-2x', 'addon-native-2x', 'addon-3x', 'addon-4x')]
  [string]$Case,

  [ValidateRange(1, 10)]
  [int]$Run = 1,

  [ValidateRange(30, 120)]
  [int]$DurationSeconds = 45,

  [string]$ProcessName = 'Stalker2-Win64-Shipping',

  [string]$ConfigurationNotes = '',

  [string]$OutputRoot = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) `
    'MFGBenchmarks\STALKER2'),

  [string]$PresentMonPath = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) `
    'Tooling\PresentMon\PresentMon-2.4.1-x64.exe'),

  [string]$NvapiProbePath = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) `
    'Tooling\NVIDIA\nvapi_mfg_probe.exe')
)

$ErrorActionPreference = 'Stop'
$label = "stalker2-$Case-run$Run"
$capture = Join-Path $PSScriptRoot 'Capture-Performance.ps1'
if (-not (Test-Path -LiteralPath $capture -PathType Leaf)) {
  throw "Capture-Performance.ps1 was not found beside this script."
}

Write-Host ''
Write-Host "STALKER 2 frame-pacing case: $Case, run $Run"
Write-Host 'Before continuing, load the agreed save and stand at the route start.'
Write-Host 'Keep resolution, HDR, VSync/G-SYNC, frame cap, driver and DLLs unchanged.'
Write-Host 'Close the ReShade overlay during capture and repeat the same camera route.'
Write-Host 'The diagnostic companion addon must be removed for this performance run.'
if ($Case -eq 'native-no-addon-2x') {
  Write-Host 'This case requires MFG Unlock to be removed and the game fully restarted.'
} else {
  Write-Host 'Verify MFG Unlock is loaded and select the multiplier named by the case.'
}
Read-Host 'Press Enter when ready; the timed capture starts immediately'

& $capture -Label $label -DurationSeconds $DurationSeconds `
  -ProcessName $ProcessName -ConfigurationNotes $ConfigurationNotes `
  -OutputRoot $OutputRoot -PresentMonPath $PresentMonPath `
  -NvapiProbePath $NvapiProbePath
