import sys, json, math, collections

path = sys.argv[1] if len(sys.argv) > 1 else "debug-c190fb.log"
nbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 3_000_000

with open(path, "rb") as f:
    f.seek(0, 2)
    size = f.tell()
    f.seek(max(0, size - nbytes))
    blob = f.read().decode("utf-8", "ignore")

shakes = []
sums = []
skels = []
for line in blob.splitlines():
    if '"message":"' not in line:
        continue
    try:
        j = json.loads(line)
    except Exception:
        continue
    m = j.get("message")
    if m == "aim_shake":
        shakes.append(j["data"])
    elif m == "aim_shake_sum":
        sums.append(j["data"])
    elif m == "skel_lag":
        skels.append(j["data"])

def stat(vals, fmt="{:.1f}"):
    if not vals:
        return "n/a"
    v = sorted(vals)
    return f"n={len(v)} med={fmt.format(v[len(v)//2])} p90={fmt.format(v[int(len(v)*0.9)])} max={fmt.format(v[-1])}"

print("=== AIM SHAKE (per crossover event) ===")
if shakes:
    dist = [s.get("distPx", -1) for s in shakes]
    close = [s.get("closeFrac", -1) for s in shakes]
    pxm = [s.get("pxPerMouse", -1) for s in shakes]
    tick = [s.get("tickMs", -1) for s in shakes]
    shake = [s.get("shakeDeg", -1) for s in shakes]
    supp = collections.Counter(s.get("suppress") for s in shakes)
    err = [math.hypot(s["err"][0], s["err"][1]) for s in shakes if isinstance(s.get("err"), list) and len(s["err"]) == 2]
    print(f"distPx:        {stat(dist)}")
    print(f"errMag:        {stat(err)}")
    print(f"closeFrac:     {stat(close, '{:.2f}')}")
    print(f"pxPerMouse:    {stat(pxm, '{:.3f}')}")
    print(f"tickMs:        {stat(tick)}")
    print(f"shakeDeg:      {stat(shake)}")
    print(f"suppress flag: {dict(supp)}")
else:
    print("no aim_shake events in window")

print("=== AIM SHAKE SUM (500ms summaries) ===")
if sums:
    fx = [s.get("flipX", 0) for s in sums]
    fy = [s.get("flipY", 0) for s in sums]
    ov = [s.get("overshoot", 0) for s in sums]
    sw = [s.get("switches", 0) for s in sums]
    tk = [s.get("tickMs", 0) for s in sums if s.get("tickMs", 0) > 0]
    print(f"flipX/500ms: {stat(fx, '{:.0f}')}   flipY/500ms: {stat(fy, '{:.0f}')}")
    print(f"overshoot:   {stat(ov, '{:.0f}')}   switches:    {stat(sw, '{:.0f}')}")
    print(f"tickMs:      {stat(tk)}")
else:
    print("no aim_shake_sum in window")

print("=== SKELETON LAG (skel_lag) ===")
if skels:
    age = [s.get("ageMs", -1) for s in skels]
    ext = [s.get("extAgeMs", -1) for s in skels if s.get("extAgeMs", -1) >= 0]
    dw = [s.get("dWcm", -1) for s in skels]
    dsp = [s.get("dSpx", -1) for s in skels]
    spd = [s.get("speed", -1) for s in skels]
    moving = [s for s in skels if s.get("speed", 0) > 150]
    still = [s for s in skels if s.get("speed", 0) <= 150]
    print(f"boneAgeMs:   {stat(age)}")
    print(f"extAgeMs:    {stat(ext)}")
    print(f"dWcm:        {stat(dw)}")
    print(f"dSpx:        {stat(dsp)}")
    if moving:
        print(f"MOVING (>150cm/s, n={len(moving)}): dWcm med={sorted(m['dWcm'] for m in moving)[len(moving)//2]:.0f} dSpx med={sorted(m['dSpx'] for m in moving)[len(moving)//2]:.0f}")
    if still:
        print(f"STILL  (n={len(still)}):       dWcm med={sorted(s['dWcm'] for s in still)[len(still)//2]:.0f} dSpx med={sorted(s['dSpx'] for s in still)[len(still)//2]:.0f}")
else:
    print("no skel_lag in window")
