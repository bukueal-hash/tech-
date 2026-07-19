param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
Write-Host '=== perf_spike by thread (this run) ==='
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match '"perf_spike"' } | ForEach-Object {
    [pscustomobject]@{ T=$rxT.Match($_).Groups[1].Value; Ms=[double]$rxMs.Match($_).Groups[1].Value }
} | Group-Object T | ForEach-Object {
    $st = $_.Group | Measure-Object Ms -Maximum -Average
    [pscustomobject]@{ Thread=$_.Name; Count=$_.Count; MaxMs=[math]::Round($st.Maximum,0); AvgMs=[math]::Round($st.Average,0) }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize

Write-Host '=== frame_build stats ==='
$rxTot = [regex]'"totalMs":(\d+)'
$vals = Get-Content $Log | Where-Object { $_ -match '"frame_build"' } | ForEach-Object { [int]$rxTot.Match($_).Groups[1].Value }
$m = $vals | Measure-Object -Maximum -Average
"count={0} avg={1} max={2}" -f $vals.Count, [math]::Round($m.Average,1), $m.Maximum
Write-Host '--- worst 5 frame_build ---'
Get-Content $Log | Where-Object { $_ -match '"frame_build"' } | Sort-Object { [int]$rxTot.Match($_).Groups[1].Value } -Descending | Select-Object -First 5

Write-Host '=== bot_retain_batch last 4 ==='
Get-Content $Log | Where-Object { $_ -match '"bot_retain_batch"' } | Select-Object -Last 4
Write-Host '=== bot_retain_defer last 4 ==='
Get-Content $Log | Where-Object { $_ -match '"bot_retain_defer"' } | Select-Object -Last 4
Write-Host '=== item_shell_batch last 3 ==='
Get-Content $Log | Where-Object { $_ -match '"item_shell_batch"' } | Select-Object -Last 3
Write-Host '=== container_open_batch last 3 ==='
Get-Content $Log | Where-Object { $_ -match '"container_open_batch"' } | Select-Object -Last 3
Write-Host '=== esp_paint worst 5 ==='
$rxP = [regex]'"paintMs":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match '"esp_paint"' } | Sort-Object { [double]$rxP.Match($_).Groups[1].Value } -Descending | Select-Object -First 5
Write-Host '=== pos_refresh worst 5 ==='
$rxR = [regex]'"ms":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match '"pos_refresh"' } | Sort-Object { [double]$rxR.Match($_).Groups[1].Value } -Descending | Select-Object -First 5
