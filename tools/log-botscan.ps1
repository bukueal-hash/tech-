$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== bot_scan drawing vs zeroPos (last 16) ==='
Select-String -Path $Log -Pattern '"message":"bot_scan"' -SimpleMatch | Select-Object -Last 16 | ForEach-Object {
  if ($_.Line -match '"admitted":(\d+),"drawing":(\d+),"cache":(\d+),"visSkip":(\d+),"distSkip":(\d+),"zeroPos":(\d+)') {
    'admitted={0} drawing={1} cache={2} visSkip={3} distSkip={4} zeroPos={5}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6]
  }
}
Write-Host '=== bot_nopos label frequency (this run) ==='
$n=@{}
Select-String -Path $Log -Pattern '"message":"bot_nopos"' -SimpleMatch | ForEach-Object {
  if ($_.Line -match '"label":"([^"]*)"') { $n[$Matches[1]] = 1 + [int]$n[$Matches[1]] }
}
$n.GetEnumerator() | Sort-Object Value -Descending | Format-Table -AutoSize
