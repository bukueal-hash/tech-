$p = 'F:\Test\ARCs\Project\Functions\EngineThreads.cpp'
$c = [System.IO.File]::ReadAllText($p)
if ($c.Contains('#include <thread>')) {
    Write-Host 'already-applied'
    exit 0
}
$c = $c.Replace('#include <string>', "#include <string>`r`n#include <thread>")
[System.IO.File]::WriteAllText($p, $c)
Write-Host 'applied'
