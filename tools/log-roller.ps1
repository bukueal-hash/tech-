param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
Write-Host '=== last 4 world_draw_caps ==='
Get-Content $Log | Where-Object { $_ -match '"world_draw_caps"' } | Select-Object -Last 4
Write-Host ''
Write-Host '=== bot_verify_fail fname histogram ==='
$rxF = [regex]'"fname":"([^"]*)"'
Get-Content $Log | Where-Object { $_ -match '"bot_verify_fail"' } | ForEach-Object {
    $rxF.Match($_).Groups[1].Value
} | Group-Object | Sort-Object Count -Descending | Select-Object -First 25 | Format-Table Name, Count -AutoSize
Write-Host '=== bot_verify_fail entries mentioning roller/ball/souvenir/fireball/bomb ==='
Get-Content $Log | Where-Object { $_ -match '"bot_verify_fail"' -and $_ -match '(?i)roll|ball|souvenir|fire|bomb|shred|queen|matriarch|bertha' } | Select-Object -First 10
Write-Host ''
Write-Host '=== bot_scan all ==='
Get-Content $Log | Where-Object { $_ -match '"bot_scan"' }
Write-Host ''
Write-Host '=== frame_build all ==='
Get-Content $Log | Where-Object { $_ -match '"frame_build"' } | Select-Object -Last 8
Write-Host ''
Write-Host '=== raid_hb last 3 ==='
Get-Content $Log | Where-Object { $_ -match '"raid_hb"' } | Select-Object -Last 3
