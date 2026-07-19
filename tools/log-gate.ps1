param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')

$rxTs = [regex]'"timestamp":([0-9]+)'

Write-Host '=== scan_gate: wait/held stats per scanner ==='
$rows = Select-String -Path $Log -Pattern '"scan_gate"' | ForEach-Object {
  $l = $_.Line
  if ($l -match '"scanner":"([^"]+)","waitMs":([0-9]+),"heldMs":([0-9]+),"waiters":([0-9]+),"blockedBy":"([^"]*)"') {
    [pscustomobject]@{ Scanner=$Matches[1]; WaitMs=[int]$Matches[2]; HeldMs=[int]$Matches[3]; Waiters=[int]$Matches[4]; BlockedBy=$Matches[5] }
  }
}
if ($rows) {
  $rows | Group-Object Scanner | ForEach-Object {
    $w = $_.Group | Measure-Object WaitMs -Average -Maximum
    $h = $_.Group | Measure-Object HeldMs -Average -Maximum
    [pscustomobject]@{ Scanner=$_.Name; N=$_.Count; AvgWait=[math]::Round($w.Average,0); MaxWait=$w.Maximum; AvgHeld=[math]::Round($h.Average,0); MaxHeld=$h.Maximum }
  } | Format-Table -AutoSize
  Write-Host '--- blockedBy distribution ---'
  $rows | Group-Object BlockedBy | Sort-Object Count -Descending | Select-Object Count,Name | Format-Table -AutoSize
}

Write-Host '=== perf_spike by thread ==='
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Select-String -Path $Log -Pattern '"perf_spike"' | ForEach-Object {
  $l = $_.Line
  [pscustomobject]@{ T=$rxT.Match($l).Groups[1].Value; Ms=[double]$rxMs.Match($l).Groups[1].Value }
} | Group-Object T | ForEach-Object {
  $s = $_.Group | Measure-Object Ms -Average -Maximum
  [pscustomobject]@{ Thread=$_.Name; Count=$_.Count; AvgMs=[math]::Round($s.Average,0); MaxMs=$s.Maximum }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize

Write-Host '=== pos_refresh worst 5 ==='
Select-String -Path $Log -Pattern '"pos_refresh"' | ForEach-Object {
  $l = $_.Line
  if ($l -match '"ms":([0-9.]+)') { [pscustomobject]@{ Ms=[double]$Matches[1]; Line=$l } }
} | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { $_.Line }

Write-Host '=== frame_build stats ==='
$fb = Select-String -Path $Log -Pattern '"frame_build"' | ForEach-Object {
  if ($_.Line -match '"totalMs":([0-9.]+)') { [double]$Matches[1] }
}
if ($fb) { $fb | Measure-Object -Average -Maximum | Format-List Count,Average,Maximum }

Write-Host '=== player_admit_ring last 5 ==='
Select-String -Path $Log -Pattern '"player_admit_ring"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_admit_batch last 3 ==='
Select-String -Path $Log -Pattern '"bot_admit_batch"' | Select-Object -Last 3 | ForEach-Object { $_.Line }
