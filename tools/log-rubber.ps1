param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
Write-Host '=== bot_nopos: unique label/fname ==='
$rxL = [regex]'"label":"([^"]*)","fname":"([^"]*)"'
Get-Content $Log | Where-Object { $_ -match '"bot_nopos"' } | ForEach-Object {
    $m = $rxL.Match($_)
    "{0} | {1}" -f $m.Groups[1].Value, $m.Groups[2].Value
} | Group-Object | Sort-Object Count -Descending | Format-Table Name, Count -AutoSize
Write-Host '=== bot_nopos first 3 raw ==='
Get-Content $Log | Where-Object { $_ -match '"bot_nopos"' } | Select-Object -First 3
Write-Host ''
Write-Host '=== world_draw_caps last 6 ==='
Get-Content $Log | Where-Object { $_ -match '"world_draw_caps"' } | Select-Object -Last 6
Write-Host ''
Write-Host '=== bot_quickfail_mesh: non-spawner/volume/static fnames ==='
$rxF = [regex]'"fname":"([^"]*)"'
Get-Content $Log | Where-Object { $_ -match '"bot_quickfail_mesh"' } | ForEach-Object {
    $rxF.Match($_).Groups[1].Value
} | Where-Object { $_ -notmatch '(?i)spawner|volume|staticmesh|playerstart|ddgi|indoors|outdoors' } |
    Group-Object | Sort-Object Count -Descending | Format-Table Name, Count -AutoSize
Write-Host '=== perf_spike by thread ==='
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match '"perf_spike"' } | ForEach-Object {
    [pscustomobject]@{ T=$rxT.Match($_).Groups[1].Value; Ms=[double]$rxMs.Match($_).Groups[1].Value }
} | Group-Object T | ForEach-Object {
    $st = $_.Group | Measure-Object Ms -Maximum -Average
    [pscustomobject]@{ Thread=$_.Name; Count=$_.Count; MaxMs=[math]::Round($st.Maximum,0); AvgMs=[math]::Round($st.Average,0) }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize
Write-Host '=== frame_build worst 5 ==='
$rxTot = [regex]'"totalMs":(\d+)'
Get-Content $Log | Where-Object { $_ -match '"frame_build"' } | Sort-Object { [int]$rxTot.Match($_).Groups[1].Value } -Descending | Select-Object -First 5
