param(
  [string]$Log = 'f:\Test\ARCs\debug-c190fb.log',
  [long]$Cut = 1784371560000
)

$rxT  = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
$rxTs = [regex]'"timestamp":([0-9]+)'

Write-Host "=== perf_spike by thread (timestamp >= $Cut) ==="
Select-String -Path $Log -Pattern '"perf_spike"' | ForEach-Object {
  $l = $_.Line
  $ts = [long]$rxTs.Match($l).Groups[1].Value
  if ($ts -lt $Cut) { return }
  [pscustomobject]@{ T = $rxT.Match($l).Groups[1].Value; Ms = [double]$rxMs.Match($l).Groups[1].Value }
} | Group-Object T | ForEach-Object {
  $s = $_.Group | Measure-Object Ms -Average -Maximum
  [pscustomobject]@{ Thread = $_.Name; Count = $_.Count; AvgMs = [math]::Round($s.Average, 0); MaxMs = $s.Maximum }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize

Write-Host '=== player_admit_ring last 5 ==='
Select-String -Path $Log -Pattern '"player_admit_ring"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== frame_build (this run) stats + worst 5 ==='
$fb = Select-String -Path $Log -Pattern '"frame_build"' | ForEach-Object {
  $l = $_.Line
  $ts = [long]$rxTs.Match($l).Groups[1].Value
  if ($ts -lt $Cut) { return }
  if ($l -match '"totalMs":([0-9.]+)') { [pscustomobject]@{ Ms = [double]$Matches[1]; Line = $l } }
}
if ($fb) {
  $fb | Measure-Object Ms -Average -Maximum | Format-List Count, Average, Maximum
  $fb | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { $_.Line }
}

Write-Host '=== bot_admit_batch last 3 (this run) ==='
Select-String -Path $Log -Pattern '"bot_admit_batch"' | ForEach-Object {
  $l = $_.Line
  $ts = [long]$rxTs.Match($l).Groups[1].Value
  if ($ts -ge $Cut) { $l }
} | Select-Object -Last 3

Write-Host '=== pos_refresh worst 5 (this run) ==='
Select-String -Path $Log -Pattern '"pos_refresh"' | ForEach-Object {
  $l = $_.Line
  $ts = [long]$rxTs.Match($l).Groups[1].Value
  if ($ts -lt $Cut) { return }
  if ($l -match '"ms":([0-9.]+)') { [pscustomobject]@{ Ms = [double]$Matches[1]; Line = $l } }
} | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { $_.Line }
