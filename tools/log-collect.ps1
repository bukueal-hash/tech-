$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== player_collect (last 12) ==='
Select-String -Path $Log -Pattern '"message":"player_collect"' -SimpleMatch | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Host '=== paint_frame (last 12) ==='
Select-String -Path $Log -Pattern '"message":"paint_frame"' -SimpleMatch | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Host '=== esp_paint (last 12) ==='
Select-String -Path $Log -Pattern '"message":"esp_paint"' -SimpleMatch | Select-Object -Last 12 | ForEach-Object { $_.Line }
