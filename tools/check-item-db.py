import json, io

d = json.load(io.open(r'F:\Test\ARCs\Project\Data\items_meta.json', encoding='utf-8'))
names = []

def walk(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k.lower() in ('name', 'displayname', 'display_name') and isinstance(v, str):
                names.append(v)
            walk(v)
    elif isinstance(o, list):
        for x in o:
            walk(x)

walk(d)
low = [n.lower() for n in names]
for probe in ['steel spring', 'canister', 'battery', 'jetengine', 'light drone', 'ruined parachute']:
    print(probe, '->', any(probe in n for n in low))
print('total names:', len(names))
