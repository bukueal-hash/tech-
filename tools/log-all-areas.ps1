$Log = 'F:\Test\ARCs\debug-c190fb.log'
if (-not (Test-Path $Log)) { Write-Host 'LOG MISSING'; exit 1 }

Write-Host '=== raid state ==='
Select-String -Path $Log -Pattern 'raid_hb' -SimpleMatch | Select-Object -Last 2 | ForEach-Object {
  if ($_.Line -match '"active":(\d+).*"reason":"([^"]*)".*"wName":"([^"]*)"') {
    'active={0} reason={1} wName={2}' -f $Matches[1], $Matches[2], $Matches[3]
  }
}

Write-Host '=== flicker_score (last 5) ==='
Select-String -Path $Log -Pattern 'flicker_score' -SimpleMatch | Select-Object -Last 5 | ForEach-Object {
  if ($_.Line -match '"bots":(\d+),"players":(\d+),"world":(\d+),"posFail":(\d+),"visMiss":(\d+),"evictReadmit":(\d+),"distEdge":(\d+),"other":(\d+),"fixN":(\d+)') {
    'bots={0} players={1} world={2} posFail={3} visMiss={4} evict={5} dist={6} other={7} fixN={8}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6],$Matches[7],$Matches[8],$Matches[9]
  }
}

Write-Host '=== container_admit_ring (last 4) ==='
Select-String -Path $Log -Pattern 'container_admit_ring' -SimpleMatch | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(300,$_.Line.Length)) }

Write-Host '=== container_admit (last 4) ==='
Select-String -Path $Log -Pattern '"message":"container_admit"' -SimpleMatch | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(300,$_.Line.Length)) }

Write-Host '=== world_draw_caps (last 4) ==='
Select-String -Path $Log -Pattern 'world_draw_caps' -SimpleMatch | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(320,$_.Line.Length)) }

Write-Host '=== bot_scan (last 3) ==='
Select-String -Path $Log -Pattern '"message":"bot_scan"' -SimpleMatch | Select-Object -Last 3 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(300,$_.Line.Length)) }

Write-Host '=== player_collect (last 2) ==='
Select-String -Path $Log -Pattern 'player_collect' -SimpleMatch | Select-Object -Last 2 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(260,$_.Line.Length)) }

Write-Host '=== item_shell_batch (last 3) ==='
Select-String -Path $Log -Pattern 'item_shell_batch' -SimpleMatch | Select-Object -Last 3 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(300,$_.Line.Length)) }

Write-Host '=== scan_gate worst holds (last 6 over 200ms) ==='
Select-String -Path $Log -Pattern 'scan_gate' -SimpleMatch | ForEach-Object {
  if ($_.Line -match '"scanner":"([^"]+)","waitMs":(\d+),"heldMs":(\d+)') {
    if ([int]$Matches[3] -ge 200) { '{0} held={1}ms wait={2}ms' -f $Matches[1], $Matches[3], $Matches[2] }
  }
} | Select-Object -Last 6

Write-Host '=== perf_spike (last 4) ==='
Select-String -Path $Log -Pattern 'perf_spike' -SimpleMatch | Select-Object -Last 4 | ForEach-Object {
  if ($_.Line -match '"thread":"([^"]+)","ms":(\d+)') { '{0} {1}ms' -f $Matches[1], $Matches[2] }
}

Write-Host '=== message counts ==='
$c=@{}
Get-Content $Log | ForEach-Object {
  if ($_ -match '"message":"([a-z_0-9]+)"') { $c[$Matches[1]] = 1 + [int]$c[$Matches[1]] }
}
$c.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 18 | Format-Table -AutoSize
