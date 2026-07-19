param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
Write-Host '=== bot_retain_defer last 8 ==='
Select-String -Path $Log -Pattern '"bot_retain_defer"' | Select-Object -Last 8 | ForEach-Object { $_.Line }
Write-Host '=== RobotList perf avg ==='
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Select-String -Path $Log -Pattern '"perf_spike"' | ForEach-Object {
  $l = $_.Line
  [pscustomobject]@{ T=$rxT.Match($l).Groups[1].Value; Ms=[double]$rxMs.Match($l).Groups[1].Value }
} | Group-Object T | ForEach-Object {
  $st = $_.Group | Measure-Object Ms -Maximum -Average
  [pscustomobject]@{ Thread=$_.Name; Count=$_.Count; MaxMs=[math]::Round($st.Maximum,0); AvgMs=[math]::Round($st.Average,0) }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize
Write-Host '=== bot_scan last 3 ==='
Select-String -Path $Log -Pattern '"bot_scan"' | Select-Object -Last 3 | ForEach-Object { $_.Line }
Write-Host '=== name_trace_bot last 5 ==='
Select-String -Path $Log -Pattern '"name_trace_bot"|Jetengine|jetengine|Light Drone' | Select-Object -Last 10 | ForEach-Object { $_.Line }
