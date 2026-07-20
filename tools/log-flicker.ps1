$Log = 'F:\Test\ARCs\debug-c190fb.log'

Write-Host '=== message distribution (this run, ts > 1784468900000) ==='
$counts = @{}
Get-Content $Log | ForEach-Object {
  if ($_ -match '"message":"([^"]+)"' ) {
    $m = $Matches[1]
    if ($_ -match '"timestamp":(\d+)' -and [long]$Matches[1] -gt 1784468900000) {
      $counts[$m] = 1 + ($counts[$m] | ForEach-Object { $_ })
    }
  }
}
$counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 25 | Format-Table -AutoSize

Write-Host '=== frame_build bot/player/world count sequence (last 40) ==='
Select-String -Path $Log -Pattern '"message":"frame_build"' | Select-Object -Last 40 | ForEach-Object {
  if ($_.Line -match '"buildMs":(\d+).*"players":(\d+),"bots":(\d+),"world":(\d+).*"timestamp":(\d+)') {
    '{0}  build={1}ms players={2} bots={3} world={4}' -f $Matches[5], $Matches[1], $Matches[2], $Matches[3], $Matches[4]
  }
}
