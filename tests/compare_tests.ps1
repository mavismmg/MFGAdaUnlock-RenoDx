# SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'
$output = & "$PSScriptRoot\..\src\addons\mfgdiagnostics\Compare-HdrCaptures.ps1" `
    -Baseline "$PSScriptRoot\fixtures\hdr-off.json" -Candidate "$PSScriptRoot\fixtures\hdr-on.json"
$text = $output -join "`n"
if ($text -notmatch 'HDR10/PQ' -or $text -notmatch 'SDR/sRGB' -or
    $text -notmatch '"HDRFormatFailureSamples":\s*1' -or
    $text -notmatch '"HDRFormatFailureSamples":\s*0' -or
    $text -notmatch '"DriverObservedFGMultiplier":\s*2' -or
    $text -notmatch '"ValidLatencyFrames":\s*1' -or
    $text -notmatch '"Field":\s*"OutputModes"') {
    throw 'Capture summary/SDR-PQ/NVAPI comparison did not match synthetic fixture expectations.'
}
Write-Output 'PASS: capture summary, color-space/status decoding, NVAPI summary and empty input groups.'
