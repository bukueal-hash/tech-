import sys, json, math, collections

path = sys.argv[1] if len(sys.argv) > 1 else "debug-c190fb.log"
nbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 3_000_000

with open(path, "rb") as f:
    f.seek(0, 2)
    size = f.tell()
    f.seek(max(0, size - nbytes))
    blob = f.read().decode("utf-8", "ignore")

stats = collections.defaultdict(list)
dpz = []
parsed = failed = 0
for line in blob.splitlines():
    if "align_probe" not in line:
        continue
    try:
        j = json.loads(line)
        parsed += 1
    except Exception as e:
        failed += 1
        if failed <= 2:
            print(f"PARSE FAIL: {e}: {line[:120]!r}")
        continue
    if j.get("message") != "align_probe":
        continue
    d = j["data"]
    bx, by = d["box"][0], d["box"][1]
    px, py = d["pelv"][0], d["pelv"][1]
    xy = math.hypot(px - bx, py - by)
    stats[d["mv"]].append(xy)
    dpz.append(d["dPz"])

print(f"parsed={parsed} failed={failed}")
for mv in (0, 1):
    v = sorted(stats[mv])
    if v:
        print(f"mv={mv} n={len(v)} median={v[len(v)//2]:.0f}cm p90={v[int(len(v)*0.9)]:.0f}cm max={v[-1]:.0f}cm")
    else:
        print(f"mv={mv} no samples")
if dpz:
    dpz.sort()
    print(f"dPz pelvis-above-box: median={dpz[len(dpz)//2]:.0f}cm min={dpz[0]:.0f} max={dpz[-1]:.0f}")
