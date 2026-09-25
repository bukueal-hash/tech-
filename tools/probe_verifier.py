import io, re
path = r'F:\Test\ARCs\scripts\build.ps1'
s = io.open(path, encoding='utf-8-sig').read()
lines = s.split('\n')
out = []
# find Get-BuildProblems span
st = s.find('function Get-BuildProblems')
assert st != -1, 'no GBP'
start_ln = 1 + s[:st].count('\n')
# find the function-end: next line that is exactly '}' at column 0 after start
body = s[st:]
m = re.search(r'\n}\s*\n', body)
end_off = body.find('\n}\n') + 1
end_ln = start_ln + body[:end_off].count('\n')
out.append('GBP_FUNC %d..%d' % (start_ln, end_ln))
# find all ContainsKey usages inside function with context
seg = '\n'.join(lines[start_ln-1:end_ln])
for i, ln in enumerate(seg.split('\n'), start=start_ln):
    if 'ContainsKey' in ln or 'owned' in ln.lower() or 'changed although' in ln or 'was linked but' in ln or 'Owed' in ln:
        out.append('%d:%s' % (i, ln.rstrip()))
open(r'F:\Test\ARCs\tools\probe_result.txt', 'w', encoding='utf-8').write('\n'.join(out))
print('WROTE %d' % len(out))
