$ErrorActionPreference = 'Continue'
Write-Host '[*] Elevated kill ArcRaiders + clear tlog locks...'
Get-Process -Name ArcRaiders -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
foreach ($p in @('MSBuild','cl','mspdbsrv','vctip','Tracker','link')) {
    Get-Process -Name $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 1
Remove-Item 'F:\Test\ARCs\Build\Intermediate\Project.tlog' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'F:\Test\ARCs\Build\Intermediate\ContainerList.obj' -Force -ErrorAction SilentlyContinue
if (Get-Process -Name ArcRaiders -ErrorAction SilentlyContinue) {
    Write-Host '[!] ArcRaiders still running'
    exit 2
}
Write-Host '[+] ArcRaiders stopped'
& 'F:\Test\ARCs\rebuild-run.ps1' -BuildOnly
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& 'F:\Test\ARCs\rebuild-run.ps1' -RunOnly
exit $LASTEXITCODE
