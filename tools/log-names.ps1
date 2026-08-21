$Log = 'F:\Test\ARCs\debug-c190fb.log'
Write-Host '=== name_trace_world (last 25) ==='
Select-String -Path $Log -Pattern '"message":"name_trace_world"' -SimpleMatch | Select-Object -Last 25 | ForEach-Object { $_.Line }
Write-Host '=== any log line containing Carousel/WBP/Announcement ==='
Select-String -Path $Log -Pattern 'Carousel|WBP|Announcement' | Select-Object -Last 20 | ForEach-Object { $_.Line }
Write-Host '=== container_admit label distribution (last 400) ==='
$labels = @{}
Select-String -Path $Log -Pattern '"message":"container_admit"' -SimpleMatch | Select-Object -Last 400 | ForEach-Object {
  if ($_.Line -match '"label":"([^"]*)"') { $l=$Matches[1]; $labels[$l] = 1 + [int]$labels[$l] }
}
$labels.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 30 | Format-Table -AutoSize
