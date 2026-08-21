$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== bot_nopos Snitch/Surveyor with liveRoot (last 8) ==='
Select-String -Path $Log -Pattern 'bot_nopos' -SimpleMatch | Where-Object { $_.Line -match 'Snitch|Surveyor' } | Select-Object -Last 8 | ForEach-Object {
  if ($_.Line -match '"label":"([^"]*)".*"root":(\d+),"mesh":(\d+).*"liveRoot":(\d+),"liveRootX":(-?\d+),"liveRootZ":(-?\d+)') {
    'label={0} root={1} mesh={2} liveRoot={3} liveRootX={4} liveRootZ={5}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6]
  } else { $_.Line }
}
