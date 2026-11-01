[CmdletBinding(DefaultParameterSetName = 'Case')]
param(
  [Parameter(Mandatory = $true)][ValidateSet('cp', 'cd')][string]$Game,

  [Parameter(ParameterSetName = 'Case', Mandatory = $true)]
  [ValidateSet('off', 'xefg2x-18', 'xefg2x-16', 'xefg4x', 'xefg6x', 'dlssg4x', 'xmx2x', 'xmx4x', 'xmx6x')]
  [string]$Case,

  # With -Case: measures the running game instead of changing its configuration.
  [Parameter(ParameterSetName = 'Case')][ValidateSet('1440p', '4k')][string]$Capture,
  [Parameter(ParameterSetName = 'Case')][ValidateRange(10, 300)][int]$DurationSeconds = 30,

  [Parameter(ParameterSetName = 'Install', Mandatory = $true)][switch]$Install,
  [Parameter(ParameterSetName = 'Restore', Mandatory = $true)][switch]$Restore,

  # Tests point this at a copy of the game folder.
  [string]$GameDir = ''
)
# Field test profiles of plan 1.8 (docs/plan/roteiro-1.8.md).
$ErrorActionPreference = 'Stop'

$steam = 'C:\Program Files (x86)\Steam\steamapps\common'
$games = @{
  cp = @{ Dir = "$steam\Cyberpunk 2077\bin\x64"; Process = 'Cyberpunk2077.exe' }
  cd = @{ Dir = "$steam\Crimson Desert\bin64"; Process = 'CrimsonDesert.exe' }
}
$target = $games[$Game]
$dir = if ($GameDir) { $GameDir } else { $target.Dir }
$repo = Split-Path $PSScriptRoot -Parent
$files = 'MFGAdaUnlock.ini', 'ReShade.ini', 'renodx-mfgunlock.addon64'
$suffix = '.before-1.8-field.bak'

if ($Capture) {
  $timing = if ($Case -like 'xefg*') { 'xemfg' } else { 'native' }
  & (Join-Path $repo 'src\addons\mfgdiagnostics\Capture-Performance.ps1') `
    -Label "$Game-$Capture-$Case" -ProcessName $target.Process -DurationSeconds $DurationSeconds `
    -TimingPath $timing -ConfigurationNotes "plan 1.8 field test: $Game $Capture $Case"
  return
}

$running = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($target.Process)) -ErrorAction SilentlyContinue
if ($running -and -not $GameDir) { throw "Close the game first: it reads these files at startup." }

if ($Restore) {
  foreach ($file in $files) {
    $backup = Join-Path $dir "$file$suffix"
    if (-not (Test-Path -LiteralPath $backup)) { throw "No backup to restore: $backup" }
    Copy-Item -LiteralPath $backup (Join-Path $dir $file) -Force
  }
  "Restored $dir"
  return
}

if ($Install) {
  foreach ($file in $files) {
    $backup = Join-Path $dir "$file$suffix"
    # Never over an earlier backup: that one holds the state from before the tests.
    if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath (Join-Path $dir $file) $backup }
  }
  $addon = Join-Path $repo 'build\tests\xemfg\Release\renodx-mfgunlock-native.addon64'
  Copy-Item -LiteralPath $addon (Join-Path $dir 'renodx-mfgunlock.addon64') -Force
  "Installed $((Get-FileHash -LiteralPath $addon -Algorithm SHA256).Hash.Substring(0, 16)) in $dir"
  return
}

Add-Type -Namespace MfgField -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);
'@
function Set-Ini([string]$File, [string]$Section, [string]$Key, [string]$Value) {
  $path = Join-Path $dir $File
  if (-not [MfgField.Ini]::WritePrivateProfileStringW($Section, $Key, $Value, $path)) {
    throw "Could not write $Key in $path"
  }
}

$backend = 'DLSSG'; $xefg = '4'; $stock = '0'; $force = '0'; $xmx = '0'
switch -Regex ($Case) {
  '^xefg(\d)x' { $backend = 'XeMFG'; $xefg = $Matches[1] }
  '^xefg2x-18$' { $stock = '1' }
  '^dlssg(\d)x$' { $force = $Matches[1] }
  '^xmx(\d)x$' { $force = $Matches[1]; $xmx = '1' }
}
Set-Ini 'MFGAdaUnlock.ini' 'Backend' 'Selected' $backend
Set-Ini 'MFGAdaUnlock.ini' 'XeFG' 'Multiplier' $xefg
Set-Ini 'MFGAdaUnlock.ini' 'XeFG' 'XeFGStock2xModel' $stock
Set-Ini 'MFGAdaUnlock.ini' 'DLSSG' 'XmxGenerator' $xmx
Set-Ini 'ReShade.ini' 'RenoDX.MFGUnlock' 'ForceMultiplier' $force
$generation = if ($Case -eq 'off') { 'OFF' } else { 'ON' }
"$Game ${Case}: backend $backend, XeFG $xefg x, stock model $stock, ForceMultiplier $force, XMX $xmx. Frame generation in the game menu: $generation."
