param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
Write-Host '=== name_trace_world: suspicious fnames with labels ==='
Get-Content $Log | Where-Object { $_ -match '"name_trace_world"' } | Where-Object {
    $_ -match 'Spillway|Tower|Ruins|Spawner|Exclusion|AudioComponent|Carry|Puzzle|CargoShip|Husk|Probe|SupplyCall|Fertilizer|DBNO'
}
Write-Host ''
Write-Host '=== bot_grace evict events: which fnames ==='
$rxF = [regex]'"fname":"([^"]*)"'
Get-Content $Log | Where-Object { $_ -match '"bot_grace"' -and $_ -match '"event":"evict"' } | ForEach-Object {
    $rxF.Match($_).Groups[1].Value
} | Group-Object | Sort-Object Count -Descending | Format-Table Name, Count -AutoSize
