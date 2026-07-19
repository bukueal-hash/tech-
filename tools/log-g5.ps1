$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== bot_grace spared vs evict counts ==='
$spared = (Select-String -Path $Log -Pattern '"event":"spared"' -SimpleMatch).Count
$evict = (Select-String -Path $Log -Pattern '"event":"evict"' -SimpleMatch).Count
"spared=$spared evict=$evict"
Write-Host '=== evict events (last 8) ==='
Select-String -Path $Log -Pattern '"event":"evict"' -SimpleMatch | Select-Object -Last 8 | ForEach-Object { $_.Line }
Write-Host '=== spared events (last 8) ==='
Select-String -Path $Log -Pattern '"event":"spared"' -SimpleMatch | Select-Object -Last 8 | ForEach-Object { $_.Line }
Write-Host '=== item_posgate_drop (last 10) ==='
Select-String -Path $Log -Pattern '"message":"item_posgate_drop"' -SimpleMatch | Select-Object -Last 10 | ForEach-Object { $_.Line }
Write-Host '=== pos_refresh spikes >100ms count ==='
(Select-String -Path $Log -Pattern '"message":"pos_refresh"' -SimpleMatch | Where-Object { $_.Line -match '"ms":(\d+)' -and [int]$Matches[1] -gt 100 }).Count
