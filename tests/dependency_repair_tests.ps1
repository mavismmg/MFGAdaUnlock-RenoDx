# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/Repair-XeDependencies.ps1')
function Check($value,[string]$message) { if(!$value) { throw $message } }
function Reject([scriptblock]$action,[string]$message) { $rejected=$false; try { & $action } catch { $rejected=$true }; Check $rejected $message }
$root=[IO.Path]::GetFullPath((Join-Path $TestRoot ([guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $root | Out-Null
$game=Join-Path $root 'game';$cache=Join-Path $root 'cache'
New-Item -ItemType Directory -Path $game,$cache | Out-Null
$target=Join-Path $game 'libxell.dll';$stage=Join-Path $cache 'new.dll'
[IO.File]::WriteAllText($target,'original');[IO.File]::WriteAllText($stage,'new verified fixture')
$before=Get-XeDigest $target;$after=Get-XeDigest $stage
$plan=@(@{target=$target;before=$before;after=$after;stage=$stage})
$closed={}
$journal=Invoke-XeTransaction $game $plan $closed
Check ((Get-XeDigest $target) -eq $after) 'installed fixture differs'
Restore-XeTransaction $journal $closed
Check ((Get-XeDigest $target) -eq $before) 'rollback differs'
Reject { Invoke-XeTransaction $game $plan {throw 'game running'} } 'running game accepted'
[IO.File]::WriteAllText($target,'foreign edit')
Reject { Invoke-XeTransaction $game $plan $closed } 'changed target replaced'
[IO.File]::WriteAllText($target,'original')
[IO.File]::WriteAllText($stage,'bad download')
Reject { Invoke-XeTransaction $game $plan $closed } 'wrong download hash installed'
[IO.File]::WriteAllText($stage,'new verified fixture')
Reject { Assert-XePath $game (Join-Path $root 'escape.dll') } 'outside target accepted'
$newTarget=Join-Path $game 'libxess_fg.dll'
$newPlan=@(@{target=$newTarget;before=$null;after=$after;stage=$stage})
$newJournal=Invoke-XeTransaction $game $newPlan $closed
Restore-XeTransaction $newJournal $closed
Check (!(Test-Path -LiteralPath $newTarget)) 'new installation not removed on rollback'
# Failure after one successful replacement must roll back that first file.
$second=Join-Path $game 'locked.dll'; [IO.File]::WriteAllText($second,'original')
$both=@($plan[0],@{target=$second;before=$before;after=$after;stage=$stage})
$handle=[IO.File]::Open($second,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try { Reject { Invoke-XeTransaction $game $both $closed } 'locked target installed' } finally { $handle.Dispose() }
Check ((Get-XeDigest $target) -eq $before -and (Get-XeDigest $second) -eq $before) 'partial failure did not roll back'
# A prepared journal can be resumed after a crash before state advancement.
$journal=Invoke-XeTransaction $game $plan $closed
$j=Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json;$j.state='prepared';Write-XeJournal $j $journal
Restore-XeTransaction $journal $closed
Check ((Get-XeDigest $target) -eq $before) 'interrupted transaction did not recover'
$journal=Invoke-XeTransaction $game $plan $closed
[IO.File]::WriteAllText($target,'foreign edit after install')
Reject { Restore-XeTransaction $journal $closed } 'rollback overwrote foreign change'
# Store/Game Pass: the process reports a WindowsApps junction to the real install.
$real=Join-Path $root 'XboxGames\Game\Content'; New-Item -ItemType Directory -Path $real | Out-Null
$link=Join-Path $root 'WindowsApps-package'; New-Item -ItemType Junction -Path $link -Target $real | Out-Null
Check ((Resolve-XeFinalPath $link) -eq $real) 'junction was not resolved to the real install'
Reject { Assert-XePath $link (Join-Path $link 'libxell.dll') } 'write through a junction accepted'
[void](Assert-XePath (Resolve-XeFinalPath $link) (Join-Path $real 'libxell.dll'))
Check ((Find-XeInstallRoot $real) -eq $real) 'Xbox Content root not detected'
Check ((Find-XeInstallRoot (Join-Path $real 'Binaries\Win64')) -eq $real) 'nested Xbox executable root not detected'
$steam=Join-Path $root 'steamapps\common\Game'
Check ((Find-XeInstallRoot (Join-Path $steam 'bin\x64')) -eq $steam) 'Steam root not detected'
Check ((Find-XeInstallRoot $steam) -eq $steam) 'Steam root executable folder changed'
$unreal=Join-Path $root 'Store\Game'
Check ((Find-XeInstallRoot (Join-Path $unreal 'Project\Binaries\Win64')) -eq $unreal) 'Unreal root not detected'
$other=Join-Path $root 'Other\Game'
Check ((Find-XeInstallRoot $other) -eq $other) 'unknown layout widened the search scope'
# Engine plugin copies are found; junctions and helper backups are not followed.
$exe=Join-Path $steam 'bin\x64'; $plugin=Join-Path $steam 'Engine\Plugins\XeSS\Binaries\Win64'
$backup=Join-Path $steam '.mfg-dependency-backups\old'; $outside=Join-Path $root 'outside'
New-Item -ItemType Directory -Path $exe,$plugin,$backup,$outside | Out-Null
foreach($p in @((Join-Path $plugin 'libxess_fg.dll'),(Join-Path $backup 'libxess_fg.dll'),(Join-Path $outside 'libxell.dll'))) { [IO.File]::WriteAllText($p,'copy') }
New-Item -ItemType Junction -Path (Join-Path $steam 'linked') -Target $outside | Out-Null
$scan=Find-XeRuntimeCopies $steam @('libxess_fg.dll','libxell.dll')
Check ($scan.complete -and @($scan.files).Count -eq 1 -and $scan.files[0] -eq (Join-Path $plugin 'libxess_fg.dll')) 'shipped copy search followed links/backups or missed a plugin copy'
Check (!(Find-XeRuntimeCopies $steam @('libxess_fg.dll') -Limit 2).complete) 'search limit not reported'
$targets=Get-XeRuntimeTargets $exe $steam 'libxess_fg.dll' @((Join-Path $exe 'LIBXESS_FG.dll')) $scan.files
Check (@($targets).Count -eq 2 -and $targets[0].role -eq 'game folder' -and $targets[1].role -eq 'shipped with the game') 'runtime targets not deduplicated/classified'
Reject { Get-XeRuntimeTargets $exe $steam 'libxess_fg.dll' @((Join-Path $outside 'libxess_fg.dll')) @() } 'loaded copy outside the installation planned'
Check (Test-XeWritable $exe) 'writable folder reported read-only'
$catalog=Join-Path $PSScriptRoot '../src/addons/mfgunlock/xemfg/dependency_catalog.json'
$entries=@{}; foreach($e in (Read-XeCatalog $catalog).files) { $entries[$e.name]=$e }
# A missing provider has no accepted_older; it must still be planned (STALKER 2,
# RDR2 and Elden Ring got only libxell.dll and kept missing libxess_fg.dll).
$fg=$entries['libxess_fg.dll']; $ll=$entries['libxell.dll']; $notice=$entries['Intel-XeSS-LICENSE.txt']
Check (Test-XeNeedsInstall $null $fg) 'missing libxess_fg.dll not planned'
Check (Test-XeNeedsInstall $null $ll) 'missing libxell.dll not planned'
Check (Test-XeNeedsInstall $null $notice) 'missing notice not planned'
Check (!(Test-XeNeedsInstall $fg.sha256 $fg)) 'pinned libxess_fg.dll replaced'
Check (!(Test-XeNeedsInstall $ll.accepted_older[0] $ll)) 'accepted older libxell.dll replaced'
Check (Test-XeNeedsInstall ('0'*64) $fg) 'unknown libxess_fg.dll kept'
Check (Test-XeNeedsInstall $ll.accepted_older[0] $fg) 'another file hash accepted for libxess_fg.dll'
# The helper log goes beside the backups in the game folder and keeps lines
# written before the installation root was known.
$logGame=Join-Path $root 'loggame'; New-Item -ItemType Directory -Path $logGame | Out-Null
$script:XeLog=$null; $script:XeLogBuffer.Clear()
Write-XeLog 'before the root is known'
$logPath=Open-XeLog $logGame
Write-XeLog 'after the root is known'
Check ([IO.Path]::GetDirectoryName($logPath) -eq (Join-Path $logGame '.mfg-dependency-backups') -and (Split-Path -Leaf $logPath) -like 'helper-*.log') 'helper log not written in the game folder'
$logText=[IO.File]::ReadAllText($logPath)
Check ($logText.Contains('before the root is known') -and $logText.Contains('after the root is known')) 'buffered helper log lines lost'
$script:XeLog=$null
# Only this helper's temporary folders are removed, including stale ones. A
# private folder stands in for %TEMP% so the machine's real folder is untouched.
$tempRoot=Join-Path $root 'temp'; $outsideStage=Join-Path $root 'MFG-XeHelper-outside'
$ownStage=Join-Path $tempRoot 'MFG-XeHelper-own'; $staleStage=Join-Path $tempRoot 'MFG-XeRepair-stale'
$freshStage=Join-Path $tempRoot 'MFG-XeRepair-fresh'; $otherStale=Join-Path $tempRoot 'Other-stale'
New-Item -ItemType Directory -Path $ownStage,$staleStage,$freshStage,$otherStale,$outsideStage | Out-Null
[IO.File]::WriteAllText((Join-Path $ownStage 'catalog.json'),'{}')
foreach($p in @($staleStage,$otherStale)) { (Get-Item -LiteralPath $p).LastWriteTime=(Get-Date).AddDays(-2) }
Check (!(Test-XeStagingFolder $game $tempRoot)) 'game folder accepted as helper staging'
Check (!(Test-XeStagingFolder $outsideStage $tempRoot)) 'staging outside the temporary folder accepted'
Clear-XeStaleStaging $ownStage $tempRoot
Check (!(Test-Path -LiteralPath $staleStage) -and (Test-Path -LiteralPath $freshStage) -and (Test-Path -LiteralPath $ownStage) -and (Test-Path -LiteralPath $otherStale)) 'stale staging cleanup removed the wrong folders'
Remove-XeStaging $ownStage $tempRoot
Check (!(Test-Path -LiteralPath $ownStage)) 'helper staging folder not removed'
Remove-XeStaging $outsideStage $tempRoot
Check (Test-Path -LiteralPath $outsideStage) 'folder outside the temporary folder removed'
$bad=Get-Content -LiteralPath $catalog -Raw | ConvertFrom-Json;$bad.files[0].sha256='0000'
$badPath=Join-Path $cache 'bad.json';$bad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badPath -Encoding UTF8
Reject { Read-XeCatalog $badPath } 'tampered catalog accepted'
# 1.10.1 (plan item 8): the Streamline catalog is pinned, and the archive reader takes only the listed
# entries (stored and deflated) from byte ranges, giving up when a range is refused.
$slCatalog=Join-Path $PSScriptRoot '../src/addons/mfgunlock/xemfg/streamline_catalog.json'
$sl=Read-SlCatalog $slCatalog
Check (@($sl.files).Count -eq 7 -and @($sl.files | Where-Object { $_.required }).Count -eq 6) 'Streamline catalog files'
$slBad=Get-Content -LiteralPath $slCatalog -Raw | ConvertFrom-Json;$slBad.files[2].sha256='0000'
$slBadPath=Join-Path $cache 'sl-bad.json';$slBad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $slBadPath -Encoding UTF8
Reject { Read-SlCatalog $slBadPath } 'tampered Streamline catalog accepted'
$slBad=Get-Content -LiteralPath $slCatalog -Raw | ConvertFrom-Json;$slBad.url='https://example.com/streamline.zip'
$slBad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $slBadPath -Encoding UTF8
Reject { Read-SlCatalog $slBadPath } 'Streamline catalog with another URL accepted'
Add-Type -AssemblyName System.IO.Compression
$zipPath=Join-Path $cache 'fixture.zip'
$zipStream=[IO.File]::Open($zipPath,[IO.FileMode]::Create)
$zip=New-Object IO.Compression.ZipArchive($zipStream,[IO.Compression.ZipArchiveMode]::Create)
$payload=[Text.Encoding]::ASCII.GetBytes(('streamline fixture ' * 400))
foreach($item in @(@('bin/x64/sl.common.dll',[IO.Compression.CompressionLevel]::Optimal),@('bin/x64/other.dll',[IO.Compression.CompressionLevel]::Optimal),@('license.txt',[IO.Compression.CompressionLevel]::NoCompression))) {
  $e=$zip.CreateEntry($item[0],$item[1]); $w=$e.Open(); $w.Write($payload,0,$payload.Length); $w.Dispose()
}
$zip.Dispose();$zipStream.Dispose()
$zipBytes=[IO.File]::ReadAllBytes($zipPath)
$reader={ param([long]$from,[long]$to) $slice=New-Object byte[] ($to-$from+1); [Array]::Copy($zipBytes,$from,$slice,0,$slice.Length); ,$slice }.GetNewClosure()
$read=Read-SlZipEntries $reader $zipBytes.LongLength @('bin/x64/sl.common.dll','license.txt')
Check ($read.Count -eq 2) 'archive reader returned other entries'
Check ([Convert]::ToBase64String([byte[]]$read['bin/x64/sl.common.dll']) -eq [Convert]::ToBase64String($payload)) 'deflated entry differs'
Check ([Convert]::ToBase64String([byte[]]$read['license.txt']) -eq [Convert]::ToBase64String($payload)) 'stored entry differs'
Reject { Read-SlZipEntries $reader $zipBytes.LongLength @('bin/x64/missing.dll') } 'missing archive entry accepted'
$refused={ param([long]$from,[long]$to) $null }
Check ($null -eq (Read-SlZipEntries $refused $zipBytes.LongLength @('license.txt'))) 'refused range not reported'
Write-Output 'PASS: install/rollback, new files, closed-game gate, unknown edits, hash failure, locked target, interrupted recovery, junction resolution, install roots, shipped copies, install plan, helper log, temporary cleanup, pinned catalog, Streamline catalog and archive reader'
