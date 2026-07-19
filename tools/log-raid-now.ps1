$Log = 'F:\Test\ARCs\debug-c190fb.log'
Select-String -Path $Log -Pattern 'raid_hb' -SimpleMatch | Select-Object -Last 4 | ForEach-Object {
  if ($_.Line -match '"active":(\d+).*"reason":"([^"]*)".*"wName":"([^"]*)"') {
    'active={0} reason={1} wName={2}' -f $Matches[1], $Matches[2], $Matches[3]
  }
}
Write-Host '=== recent flicker_score ==='
Select-String -Path $Log -Pattern 'flicker_score' -SimpleMatch | Select-Object -Last 6 | ForEach-Object {
  if ($_.Line -match '"bots":(\d+),"players":(\d+),"world":(\d+).*"fixN":(\d+)') {
    'bots={0} players={1} world={2} fixN={3}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4]
  }
}
