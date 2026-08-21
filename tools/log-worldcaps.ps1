$Log = 'F:\Test\ARCs\debug-c190fb.log'
Select-String -Path $Log -Pattern 'world_draw_caps' -SimpleMatch | Select-Object -Last 3 | ForEach-Object { $_.Line }
Write-Host '=== container settings from auto_config ==='
Select-String -Path 'F:\Test\ARCs\Build\auto_config.ini' -Pattern 'loot_distance|container_distance|container_range|show_world' | ForEach-Object { $_.Line }
