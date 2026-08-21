$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== R5 bot_pos_fallback (last 15) ==='
Select-String -Path $Log -Pattern '"hypothesisId":"R5"' | Select-Object -Last 15 | ForEach-Object { $_.Line }
Write-Host '=== CAM1 cam_refresh_gap (last 10) ==='
Select-String -Path $Log -Pattern '"hypothesisId":"CAM1"' | Select-Object -Last 10 | ForEach-Object { $_.Line }
Write-Host '=== pos_refresh spikes >50ms (last 15) ==='
Select-String -Path $Log -Pattern '"message":"pos_refresh"' | Where-Object { $_.Line -match '"ms":(\d+)' -and [int]$Matches[1] -gt 50 } | Select-Object -Last 15 | ForEach-Object { $_.Line }
Write-Host '=== esp_paint worst (>25ms, last 10) ==='
Select-String -Path $Log -Pattern '"message":"esp_paint"' | Where-Object { $_.Line -match '"ms":(\d+)' -and [int]$Matches[1] -gt 25 } | Select-Object -Last 10 | ForEach-Object { $_.Line }
Write-Host '=== bot_grace evict events (last 10) ==='
Select-String -Path $Log -Pattern '"event":"evict"' | Select-Object -Last 10 | ForEach-Object { $_.Line }
