$Log = 'f:\Test\ARCs\debug-c190fb.log'

Write-Host '=== container_admit_ring (first 5, last 15) ==='
$r = Select-String -Path $Log -Pattern 'container_admit_ring'
$r | Select-Object -First 5 | ForEach-Object { $_.Line }
Write-Host '---'
$r | Select-Object -Last 15 | ForEach-Object { $_.Line }

Write-Host '=== container_admit (first 5, last 8) ==='
$ca = Select-String -Path $Log -Pattern '"container_admit"'
$ca | Select-Object -First 5 | ForEach-Object { $_.Line }
Write-Host '---'
$ca | Select-Object -Last 8 | ForEach-Object { $_.Line }

Write-Host '=== world_draw_caps last 4 ==='
Select-String -Path $Log -Pattern 'world_draw_caps' | Select-Object -Last 4 | ForEach-Object { $_.Line }

Write-Host '=== bot_verify_fail last 8 ==='
Select-String -Path $Log -Pattern 'bot_verify_fail' | Select-Object -Last 8 | ForEach-Object { $_.Line }

Write-Host '=== bot_grace last 5 ==='
Select-String -Path $Log -Pattern '"bot_grace"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_nopos last 5 ==='
Select-String -Path $Log -Pattern '"bot_nopos"' | Select-Object -Last 5 | ForEach-Object { $_.Line }

Write-Host '=== bot_scan last 4 ==='
Select-String -Path $Log -Pattern '"bot_scan"' | Select-Object -Last 4 | ForEach-Object { $_.Line }

Write-Host '=== name_trace_world last 10 ==='
Select-String -Path $Log -Pattern 'name_trace_world' | Select-Object -Last 10 | ForEach-Object { $_.Line }
