$Log = 'F:\Test\ARCs\debug-c190fb.log'
if (-not (Test-Path $Log)) { Write-Host 'LOG MISSING'; exit }
$item = Get-Item $Log
Write-Host ('log size={0}KB lastWrite={1}' -f [int]($item.Length/1KB), $item.LastWriteTime)
Write-Host '=== message counts ==='
$counts = @{}
Get-Content $Log | ForEach-Object {
  if ($_ -match '"message":"([a-z_0-9]+)"') { $counts[$Matches[1]] = 1 + [int]$counts[$Matches[1]] }
}
$counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 22 | Format-Table -AutoSize
Write-Host '=== bot_scan (last 12 raw) ==='
Select-String -Path $Log -Pattern 'bot_scan' -SimpleMatch | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Host '=== bot_admit_batch (last 6) ==='
Select-String -Path $Log -Pattern 'bot_admit_batch' -SimpleMatch | Select-Object -Last 6 | ForEach-Object { $_.Line }
Write-Host '=== bot_verify_fail (last 6) ==='
Select-String -Path $Log -Pattern 'bot_verify_fail' -SimpleMatch | Select-Object -Last 6 | ForEach-Object { $_.Line }
Write-Host '=== bot_pos_freeze count ==='
(Select-String -Path $Log -Pattern 'bot_pos_freeze' -SimpleMatch | Measure-Object).Count
