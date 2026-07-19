param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')

Write-Host '=== bot_type_discovered ==='
Select-String -Path $Log -Pattern 'bot_type_discovered' | ForEach-Object { $_.Line }

Write-Host ''
Write-Host '=== perf_spike: by location, max ms ==='
$rxLoc = [regex]'"location":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match 'perf_spike' } | ForEach-Object {
    $loc = $rxLoc.Match($_).Groups[1].Value
    $ms = [double]$rxMs.Match($_).Groups[1].Value
    [pscustomobject]@{ Loc = $loc; Ms = $ms }
} | Group-Object Loc | ForEach-Object {
    $stats = $_.Group | Measure-Object Ms -Maximum -Average
    [pscustomobject]@{ Loc = $_.Name; Count = $_.Count; MaxMs = [math]::Round($stats.Maximum,1); AvgMs = [math]::Round($stats.Average,1) }
} | Sort-Object MaxMs -Descending | Format-Table -AutoSize

Write-Host '=== bot_grace: names ==='
$rxName = [regex]'"name":"([^"]*)"'
Get-Content $Log | Where-Object { $_ -match '"bot_grace"' } | ForEach-Object {
    $rxName.Match($_).Groups[1].Value
} | Group-Object | Sort-Object Count -Descending | Format-Table Name, Count -AutoSize

Write-Host '=== bot_verify_fail: names/reasons ==='
Get-Content $Log | Where-Object { $_ -match '"bot_verify_fail"' } | Select-Object -First 8
Write-Host ''
Write-Host '=== bot_admit_latency: names + latency ==='
$rxLat = [regex]'"latMs":([0-9]+)'
Get-Content $Log | Where-Object { $_ -match '"bot_admit_latency"' } | ForEach-Object {
    $n = $rxName.Match($_).Groups[1].Value
    $l = [int]$rxLat.Match($_).Groups[1].Value
    [pscustomobject]@{ Name=$n; Lat=$l }
} | Group-Object Name | ForEach-Object {
    $st = $_.Group | Measure-Object Lat -Maximum -Average
    [pscustomobject]@{ Name=$_.Name; Count=$_.Count; MaxLat=$st.Maximum; AvgLat=[math]::Round($st.Average,0) }
} | Sort-Object Count -Descending | Format-Table -AutoSize

Write-Host '=== player_ghost: sample ==='
Get-Content $Log | Where-Object { $_ -match '"player_ghost"' } | Select-Object -First 6
Write-Host ''
Write-Host '=== name_trace_world: sample ==='
Get-Content $Log | Where-Object { $_ -match '"name_trace_world"' } | Select-Object -First 10
Write-Host ''
Write-Host '=== container_verify_softmiss: sample ==='
Get-Content $Log | Where-Object { $_ -match '"container_verify_softmiss"' } | Select-Object -First 6
Write-Host ''
Write-Host '=== name_trace_bot: sample ==='
Get-Content $Log | Where-Object { $_ -match '"name_trace_bot"' } | Select-Object -First 10
