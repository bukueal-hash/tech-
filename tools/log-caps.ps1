$Log = 'f:\Test\ARCs\debug-c190fb.log'

Write-Host '=== world_draw_caps series (cache/drawing/distSkip over time) ==='
Select-String -Path $Log -Pattern 'world_draw_caps' | ForEach-Object {
  if ($_.Line -match '"cache":(\d+),"drawing":(\d+),"distSkip":(\d+),"lootDist":(\d+),"spDist":(\d+)') {
    $ts = if ($_.Line -match '"timestamp":(\d+)') { [long]$Matches[1] } else { 0 }
    '{0} cache={1} drawing={2} distSkip={3} lootDist={4} spDist={5}' -f $ts,$Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5]
  }
} | Select-Object -Last 40

Write-Host '=== nearestHidden labels/caps (unique) ==='
Select-String -Path $Log -Pattern 'world_draw_caps' | ForEach-Object {
  if ($_.Line -match '"nearestHiddenDist":(\d+),"nearestHiddenCap":(\d+),"nearestHiddenCat":(\d+),"nearestHiddenLabel":"([^"]*)"') {
    '{0}m cap={1} cat={2} {3}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4]
  }
} | Group-Object | Sort-Object Count -Descending | Select-Object Count,Name -First 20 | Format-Table -AutoSize

Write-Host '=== bot_verify_fail fname distribution ==='
Select-String -Path $Log -Pattern 'bot_verify_fail' | ForEach-Object {
  if ($_.Line -match '"fname":"([^"]*)"') { $Matches[1] }
} | Group-Object | Sort-Object Count -Descending | Select-Object Count,Name -First 20 | Format-Table -AutoSize

Write-Host '=== player_collect last 3 ==='
Select-String -Path $Log -Pattern '"player_collect"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== esp_paint last 3 ==='
Select-String -Path $Log -Pattern '"esp_paint"' | Select-Object -Last 3 | ForEach-Object { $_.Line }

Write-Host '=== world_lost / world_change / raid events ==='
Select-String -Path $Log -Pattern '"world_lost"|"world_change"|"raid_entered"|"raid_edge"' | ForEach-Object { $_.Line }
