$bytes = [IO.File]::ReadAllBytes('F:\Test\ARCs\Project\Functions\CollisionVis.cpp')
$line = 1
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($bytes[$i] -eq 10) {
        $line++
        if ($line -ge 965 -and $line -le 975) {
            $start = if ($i -ge 80) { $i - 80 } else { 0 }
            $len = [Math]::Min(160, $bytes.Length - $start)
            $s = [Text.Encoding]::ASCII.GetString($bytes, $start, $len)
            $s = $s -replace "`r", "" -replace "`n", "\\n"
            Write-Host "L$line : $s"
        }
    }
}
