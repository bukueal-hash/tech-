import json

for name in ("raid_line.txt", "align_line.txt"):
    try:
        line = open(f"tools/{name}", encoding="utf-8", errors="ignore").read().strip()
    except FileNotFoundError:
        print(f"{name}: missing")
        continue
    try:
        j = json.loads(line)
        print(f"{name}: PARSES OK -> {j.get('message')}")
    except Exception as e:
        print(f"{name}: FAILS -> {e}")
        print(f"   len={len(line)}")
        print(f"   full line:\n{line}")
