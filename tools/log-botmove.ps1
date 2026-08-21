$Log = 'f:\Test\ARCs\debug-c190fb.log'

Write-Host '=== pos_refresh spikes (>50ms) ==='
Select-String -Path $Log -Pattern '"pos_refresh"' | ForEach-Object {
  if ($_.Line -match '"ms":(\d+)') { if ([int]$Matches[1] -gt 50) { $_.Line } }
}

Write-Host '=== bot_nopos unique fname/label ==='
Select-String -Path $Log -Pattern '"bot_nopos"' | ForEach-Object {
  if ($_.Line -match '"label":"([^"]*)","fname":"([^"]*)"') { '{0} <- {1}' -f $Matches[1],$Matches[2] }
} | Group-Object | Sort-Object Count -Descending | Select-Object Count,Name | Format-Table -AutoSize

Write-Host '=== bot_retain_batch last 5 ==='
Select-String -Path $Log -Pattern '"bot_retain_batch"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_admit_batch last 5 ==='
Select-String -Path $Log -Pattern '"bot_admit_batch"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_scan last 5 ==='
Select-String -Path $Log -Pattern '"bot_scan"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_grace last 8 ==='
Select-String -Path $Log -Pattern '"bot_grace"' | Select-Object -Last 8 | ForEach-Object { $_.Line }

Write-Host '=== bot_verify_fail fname distribution ==='
Select-String -Path $Log -Pattern '"bot_verify_fail"' | ForEach-Object {
  if ($_.Line -match '"fname":"([^"]*)"') { $Matches[1] }
} | Group-Object | Sort-Object Count -Descending | Select-Object Count,Name -First 15 | Format-Table -AutoSize

Write-Host '=== bot_admit_latency all ==='
Select-String -Path $Log -Pattern '"bot_admit_latency"' | ForEach-Object { $_.Line }

Write-Host '=== container_memo_lockout all ==='
Select-String -Path $Log -Pattern '"container_memo_lockout"' | ForEach-Object { $_.Line }

Write-Host '=== frame_build worst 5 ==='
Select-String -Path $Log -Pattern '"frame_build"' | ForEach-Object {
  if ($_.Line -match '"totalMs":(\d+)') { [pscustomobject]@{ Ms=[int]$Matches[1]; Line=$_.Line } }
} | Sort-Object Ms -Descending | Select-Object -First 5 | ForEach-Object { $_.Line }
