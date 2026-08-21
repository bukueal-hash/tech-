$Log = 'F:\Test\ARCs\debug-c190fb.log'

Write-Host '=== R4 bot_nopos with source fields (last 25) ==='
Select-String -Path $Log -Pattern '"hypothesisId":"R4"' | Select-Object -Last 25 | ForEach-Object { $_.Line }

Write-Host '=== scan_gate per-scanner held stats ==='
$rows = Select-String -Path $Log -Pattern '"scan_gate"' | ForEach-Object {
  if ($_.Line -match '"scanner":"([^"]+)","waitMs":(\d+),"heldMs":(\d+)') {
    [pscustomobject]@{ Scanner=$Matches[1]; WaitMs=[int]$Matches[2]; HeldMs=[int]$Matches[3] }
  }
}
$rows | Group-Object Scanner | ForEach-Object {
  $w = $_.Group | Measure-Object WaitMs -Average -Maximum
  $h = $_.Group | Measure-Object HeldMs -Average -Maximum
  $big = ($_.Group | Where-Object { $_.HeldMs -gt 300 }).Count
  [pscustomobject]@{ Scanner=$_.Name; N=$_.Count; AvgHeld=[math]::Round($h.Average,0); MaxHeld=$h.Maximum; Over300=$big; AvgWait=[math]::Round($w.Average,0) }
} | Sort-Object MaxHeld -Descending | Format-Table -AutoSize

Write-Host '=== gate holds >300ms, which scanner ==='
$rows | Where-Object { $_.HeldMs -gt 300 } | Group-Object Scanner | Select-Object Count,Name | Format-Table -AutoSize
