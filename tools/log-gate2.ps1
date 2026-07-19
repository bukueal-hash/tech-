$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== gate holds >250ms (scanner + heldMs + blockedBy) ==='
Select-String -Path $Log -Pattern '"message":"scan_gate"' -SimpleMatch | ForEach-Object {
  if ($_.Line -match '"scanner":"([^"]+)","waitMs":(\d+),"heldMs":(\d+),"waiters":(\d+),"blockedBy":"([^"]*)"' -and [int]$Matches[3] -gt 250) {
    '{0,-14} held={1,4}ms wait={2,4}ms blockedBy={3}' -f $Matches[1],$Matches[3],$Matches[2],$Matches[5]
  }
} | Select-Object -Last 30

Write-Host ''
Write-Host '=== container_admit_ring cycles (last 12) ==='
Select-String -Path $Log -Pattern '"message":"container_admit_ring"' -SimpleMatch | Select-Object -Last 12 | ForEach-Object {
  if ($_.Line -match '"actors":(\d+),"slice":(\d+),"sliceActors":(\d+),"prioNew":(\d+),"checked":(\d+),"memoSkip":(\d+),"memoSize":(\d+),"coverMask":(\d+),"cycleMs":(\d+),"ringResets":(\d+)') {
    'actors={0} slice={1} checked={4} memoSkip={5} memoSize={6} cycleMs={8} resets={9}' -f $Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6],$Matches[7],$Matches[8],$Matches[9],$Matches[10]
  }
}

Write-Host ''
Write-Host '=== perf_spike worst per-thread (this run > 1784470200000) ==='
$sp = @{}
Select-String -Path $Log -Pattern '"message":"perf_spike"' -SimpleMatch | ForEach-Object {
  if ($_.Line -match '"thread":"([^"]+)","ms":(\d+).*"timestamp":(\d+)' -and [long]$Matches[3] -gt 1784470200000) {
    $t=$Matches[1]; $m=[int]$Matches[2]
    if (-not $sp.ContainsKey($t) -or $m -gt $sp[$t]) { $sp[$t]=$m }
  }
}
$sp.GetEnumerator() | Sort-Object Value -Descending | Format-Table -AutoSize
