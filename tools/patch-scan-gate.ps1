$p = 'F:\Test\ARCs\Project\Functions\EngineThreads.cpp'
$c = [System.IO.File]::ReadAllText($p)
if ($c.Contains('RunGatedScan("EntityList"')) {
    Write-Host 'already-applied'
    exit 0
}
$c = $c.Replace('        EntityList();', '        RunGatedScan("EntityList", [this] { EntityList(); });')
$c = $c.Replace('        RobotList();', '        RunGatedScan("RobotList", [this] { RobotList(); });')
$c = $c.Replace('        ContainerList();', '        RunGatedScan("ContainerList", [this] { ContainerList(); });')
$c = $c.Replace('        ItemList();', '        RunGatedScan("ItemList", [this] { ItemList(); });')
[System.IO.File]::WriteAllText($p, $c)
Write-Host 'applied'
