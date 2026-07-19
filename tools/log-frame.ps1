param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')

Write-Host '=== EntityList spikes over time (ms >= 150) ==='
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
$rxTs = [regex]'"timestamp":([0-9]+)'
$prev = $null
Select-String -Path $Log -Pattern '"perf_spike"' | ForEach-Object {
  $l = $_.Line
  if ($rxT.Match($l).Groups[1].Value -ne 'EntityList') { return }
  [pscustomobject]@{ Ms=[double]$rxMs.Match($l).Groups[1].Value; Ts=[long]$rxTs.Match($l).Groups[1].Value }
} | Where-Object { $_.Ms -ge 150 } | ForEach-Object {
  $gap = if ($prev) { $_.Ts - $prev } else { 0 }
  $prev = $_.Ts
  [pscustomobject]@{ Ms=$_.Ms; GapMs=$gap }
} | Format-Table -AutoSize

Write-Host '=== EntityList gap histogram (between >=150ms spikes) ==='
$prev2 = $null
$gaps = Select-String -Path $Log -Pattern '"perf_spike"' | ForEach-Object {
  $l = $_.Line
  if ($rxT.Match($l).Groups[1].Value -ne 'EntityList') { return }
  [pscustomobject]@{ Ms=[double]$rxMs.Match($l).Groups[1].Value; Ts=[long]$rxTs.Match($l).Groups[1].Value }
} | Where-Object { $_.Ms -ge 150 } | ForEach-Object {
  if ($prev2) { $_.Ts - $prev2 } ; $prev2 = $_.Ts
}
if ($gaps) { $gaps | Measure-Object -Average -Minimum -Maximum | Format-List Count,Average,Minimum,Maximum }

Write-Host '=== player_ghost last 4 ==='
Select-String -Path $Log -Pattern '"player_ghost"' | Select-Object -Last 4 | ForEach-Object { $_.Line }

Write-Host '=== player_collect last 4 ==='
Select-String -Path $Log -Pattern '"player_collect"' | Select-Object -Last 4 | ForEach-Object { $_.Line }

Write-Host '=== bot_admit_batch (P6b) last 5 ==='
Select-String -Path $Log -Pattern '"P6b"' | Select-Object -Last 5 | ForEach-Object { $_.Line }
