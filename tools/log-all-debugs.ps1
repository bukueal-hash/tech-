param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')

Write-Host '=== message counts (whole run) ==='
$rxMsg = [regex]'"message":"([^"]+)"'
Select-String -Path $Log -Pattern '"message"' | ForEach-Object {
  $rxMsg.Match($_.Line).Groups[1].Value
} | Group-Object | Sort-Object Count -Descending | Select-Object Count,Name | Format-Table -AutoSize

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

Write-Host '=== scan_gate stats ==='
$rows = Select-String -Path $Log -Pattern '"scan_gate"' | ForEach-Object {
  if ($_.Line -match '"scanner":"([^"]+)","waitMs":([0-9]+),"heldMs":([0-9]+)') {
    [pscustomobject]@{ Scanner=$Matches[1]; WaitMs=[int]$Matches[2]; HeldMs=[int]$Matches[3] }
  }
}
if ($rows) {
  $rows | Group-Object Scanner | ForEach-Object {
    $w = $_.Group | Measure-Object WaitMs -Average -Maximum
    $h = $_.Group | Measure-Object HeldMs -Average -Maximum
    [pscustomobject]@{ Scanner=$_.Name; N=$_.Count; AvgWait=[math]::Round($w.Average,0); MaxWait=$w.Maximum; AvgHeld=[math]::Round($h.Average,0); MaxHeld=$h.Maximum }
  } | Format-Table -AutoSize
}

Write-Host '=== pos_refresh all entries ==='
Select-String -Path $Log -Pattern '"pos_refresh"' | ForEach-Object { $_.Line }

Write-Host '=== frame_build stats + worst 3 ==='
$fb = Select-String -Path $Log -Pattern '"frame_build"' | ForEach-Object {
  if ($_.Line -match '"totalMs":([0-9.]+)') { [pscustomobject]@{ Ms=[double]$Matches[1]; Line=$_.Line } }
}
if ($fb) {
  $fb | Measure-Object Ms -Average -Maximum | Format-List Count,Average,Maximum
  $fb | Sort-Object Ms -Descending | Select-Object -First 3 | ForEach-Object { $_.Line }
}

Write-Host '=== player_admit_ring last 3 ==='
Select-String -Path $Log -Pattern '"player_admit_ring"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== bot_admit_batch last 3 ==='
Select-String -Path $Log -Pattern '"bot_admit_batch"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== bot_retain_batch last 2 / bot_retain_defer last 2 ==='
Select-String -Path $Log -Pattern '"bot_retain_batch"' | Select-Object -Last 2 | ForEach-Object { $_.Line }
Select-String -Path $Log -Pattern '"bot_retain_defer"' | Select-Object -Last 2 | ForEach-Object { $_.Line }

Write-Host '=== player_ghost last 3 ==='
Select-String -Path $Log -Pattern '"player_ghost"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== ally_team_ids last 2 ==='
Select-String -Path $Log -Pattern '"ally_team_ids"' | Select-Object -Last 2 | ForEach-Object { $_.Line }

Write-Host '=== item_posgate_drop last 5 ==='
Select-String -Path $Log -Pattern '"item_posgate_drop"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== container_verify_softmiss count + last 2 ==='
$cs = Select-String -Path $Log -Pattern '"container_verify_softmiss"'
Write-Host "count: $($cs.Count)"
$cs | Select-Object -Last 2 | ForEach-Object { $_.Line }

Write-Host '=== container_open_flip last 3 ==='
Select-String -Path $Log -Pattern '"container_open_flip"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== bot_nopos last 3 ==='
Select-String -Path $Log -Pattern '"bot_nopos"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== item_shell_batch last 2 / container_open_batch last 2 ==='
Select-String -Path $Log -Pattern '"item_shell_batch"' | Select-Object -Last 2 | ForEach-Object { $_.Line }
Select-String -Path $Log -Pattern '"container_open_batch"' | Select-Object -Last 2 | ForEach-Object { $_.Line }
