$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== R4 entries for real drones (non-SpotAudio) ==='
Select-String -Path $Log -Pattern '"hypothesisId":"R4"' | Where-Object { $_.Line -notmatch 'SpotAudioManager' } | ForEach-Object { $_.Line }
