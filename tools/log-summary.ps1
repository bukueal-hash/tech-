param([string]$Log = 'f:\Test\ARCs\debug-c190fb.log')
$rx = [regex]'"message":"([^"]+)"'
Get-Content $Log | ForEach-Object {
    $m = $rx.Match($_)
    if ($m.Success) { $m.Groups[1].Value }
} | Group-Object | Sort-Object Count -Descending | Format-Table Name, Count -AutoSize
