import subprocess, io
out = []
r = subprocess.run(
    ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
     '-Command', "& 'F:\\Test\\ARCs\\scripts\\build.ps1' *>&1 | Select-Object -Last 3 | Write-Output; exit $LASTEXITCODE"],
    capture_output=True, text=True)
code = r.returncode
tail = (r.stdout or '').replace('\r', '').strip().split('\n')
out.append('CODE=%d' % code)
out.append('STDERR=%s' % (r.stderr or '').replace('\r','').strip().replace('\n',' | ')[:200])
for t in tail[-3:]:
    out.append('T:' + t[:200])
io.open(r'F:\Test\ARCs\tools\build_gate_summary.txt', 'w', encoding='utf-8').write('\n'.join(out))
print('DONE')
