$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== recent messages ==='
$c=@{}
Get-Content $Log -ErrorAction SilentlyContinue | ForEach-Object {
  if ($_ -match '"message":"([a-z_0-9]+)"') { $c[$Matches[1]] = 1 + [int]$c[$Matches[1]] }
}
$c.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 25 | Format-Table -AutoSize
Write-Host '=== any flicker_score? ==='
(Select-String -Path $Log -Pattern 'flicker_score' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Host '=== bot_scan last 3 ==='
Select-String -Path $Log -Pattern 'bot_scan' -SimpleMatch | Select-Object -Last 3 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(250,$_.Line.Length)) }
