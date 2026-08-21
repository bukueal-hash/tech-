$ErrorActionPreference = 'Continue'
foreach ($p in @('MSBuild','cl','mspdbsrv','vctip','Tracker','link','VBCSCompiler')) {
    Get-Process -Name $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2
if (Test-Path 'F:\Test\ARCs\Build\Intermediate') {
    Remove-Item 'F:\Test\ARCs\Build\Intermediate' -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path 'F:\Test\ARCs\Build\Intermediate') {
    Write-Host '[!] Intermediate still present'
    exit 1
}
Write-Host '[+] Intermediate wiped'
exit 0
