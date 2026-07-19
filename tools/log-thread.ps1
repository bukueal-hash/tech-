param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
$rxT = [regex]'"thread":"([^"]+)"'
$rxMs = [regex]'"ms":([0-9.]+)'
Get-Content $Log | Where-Object { $_ -match '"perf_spike"' } | ForEach-Object {
    $t = $rxT.Match($_).Groups[1].Value
    $m = [double]$rxMs.Match($_).Groups[1].Value
    [pscustomobject]@{ T=$t; Ms=$m }
} | Group-Object T | ForEach-Object {
    $st = $_.Group | Measure-Object Ms -Maximum -Average
    [pscustomobject]@{ Thread=$_.Name; Count=$_.Count; MaxMs=[math]::Round($st.Maximum,0); AvgMs=[math]::Round($st.Average,0) }
} | Sort-Object AvgMs -Descending | Format-Table -AutoSize
