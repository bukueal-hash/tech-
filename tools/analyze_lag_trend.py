import sys, json, collections

path = sys.argv[1] if len(sys.argv) > 1 else "debug-c190fb.log"
nbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 8_000_000

with open(path, "rb") as f:
    f.seek(0, 2)
    size = f.tell()
    f.seek(max(0, size - nbytes))
    blob = f.read().decode("utf-8", "ignore")

rows = []
for line in blob.splitlines():
    if '"message":"skel_lag"' not in line:
        continue
    try:
        j = json.loads(line)
    except Exception:
        continue
    d = j["data"]
    ts = j.get("ts") or d.get("ts")
    if ts:
        rows.append((ts, d))

if not rows:
    print("no skel_lag rows")
    sys.exit(0)

t0 = rows[0][0]
buckets = collections.defaultdict(list)
for ts, d in rows:
    buckets[(ts - t0) // 60000].append(d)

print(f"samples={len(rows)} span={(rows[-1][0]-t0)/60000:.1f} min")
print("min | n | ageMed | ageP90 | extMed | dWmed | dSmed | dSp90 | speed")
for minute in sorted(buckets):
    ds = buckets[minute]
    def med(key, p90=False):
        v = sorted(x.get(key, 0) for x in ds)
        return v[int(len(v) * (0.9 if p90 else 0.5))]
    print(f"{minute:3d} | {len(ds):3d} | {med('ageMs'):6.0f} | {med('ageMs', True):6.0f} | "
          f"{med('extAgeMs'):6.0f} | {med('dWcm'):5.0f} | {med('dSpx'):5.1f} | {med('dSpx', True):5.1f} | {med('speed'):5.0f}")

# Distinct keys + age distribution tail
keys = collections.Counter(d.get("key") for _, d in rows)
print(f"distinct keys={len(keys)} top5={keys.most_common(5)}")
ages = sorted(d.get("ageMs", 0) for _, d in rows)
print(f"ageMs: p50={ages[len(ages)//2]:.0f} p90={ages[int(len(ages)*0.9)]:.0f} p99={ages[int(len(ages)*0.99)]:.0f} max={ages[-1]:.0f}")
ext = sorted(d.get("extAgeMs", 0) for _, d in rows)
over250 = sum(1 for e in ext if e > 250)
print(f"extAgeMs: p50={ext[len(ext)//2]:.0f} p90={ext[int(len(ext)*0.9)]:.0f} max={ext[-1]:.0f} | >250ms: {over250}/{len(ext)}")
