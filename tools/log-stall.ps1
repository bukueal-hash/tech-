$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== perf_spike breakdown by pass ==='
$p = @{}
Get-Content $Log | ForEach-Object {
  if ($_ -match '"message":"perf_spike"' -and $_ -match '"pass":"([^"]+)","ms":(\d+)') {
    $k = $Matches[1]; $ms = [int]$Matches[2]
    if (-not $p[$k]) { $p[$k] = @{n=0; max=0; sum=0} }
    $p[$k].n++; $p[$k].sum += $ms
    if ($ms -gt $p[$k].max) { $p[$k].max = $ms }
  }
}
$p.GetEnumerator() | ForEach-Object { '{0}: n={1} max={2}ms avg={3}ms' -f $_.Key, $_.Value.n, $_.Value.max, [int]($_.Value.sum / [math]::Max(1,$_.Value.n)) }
Write-Host '=== perf_spike first 5 raw ==='
Select-String -Path $Log -Pattern 'perf_spike' -SimpleMatch | Select-Object -First 5 | ForEach-Object { $_.Line }
Write-Host '=== perf_spike last 5 raw ==='
Select-String -Path $Log -Pattern 'perf_spike' -SimpleMatch | Select-Object -Last 5 | ForEach-Object { $_.Line }
Write-Host '=== raid_hb last 3 ==='
Select-String -Path $Log -Pattern 'raid_hb' -SimpleMatch | Select-Object -Last 3 | ForEach-Object { $_.Line }
Write-Host '=== raid_edge / world_change ==='
Select-String -Path $Log -Pattern 'raid_edge|world_change' | ForEach-Object { $_.Line }
Write-Host '=== scan_gate count ==='
(Select-String -Path $Log -Pattern 'scan_gate' -SimpleMatch | Measure-Object).Count
