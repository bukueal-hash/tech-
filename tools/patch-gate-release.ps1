$p = 'F:\Test\ARCs\Project\Functions\EngineThreads.cpp'
$c = [System.IO.File]::ReadAllText($p)
if ($c.Contains('g_lastGateRelease = std::chrono::steady_clock::now();' + "`r`n" + '    g_scanGateHolder.store(nullptr')) {
    Write-Host 'already-applied'
    exit 0
}
$old = "    fn();`r`n    g_scanGateHolder.store(nullptr, std::memory_order_relaxed);"
$new = "    fn();`r`n    g_lastGateRelease = std::chrono::steady_clock::now();`r`n    g_scanGateHolder.store(nullptr, std::memory_order_relaxed);"
if (-not $c.Contains($old)) {
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
}
if (-not $c.Contains($old)) {
    Write-Host 'pattern-not-found'
    exit 1
}
$c = $c.Replace($old, $new)
[System.IO.File]::WriteAllText($p, $c)
Write-Host 'applied'
