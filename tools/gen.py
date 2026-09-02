import json, math, sys

HOME_LAT, HOME_LON = 51.0447, -114.0719
CLIP_KM = 16.5
LON_SCALE = math.cos(math.radians(HOME_LAT))
KM_PER_DEG = 111.32

# Render classes. These values ARE the MAP_* enum emitted into
# roads_data.h, and ascending order is also the painter's order.
WATER, MINOR, ROAD, MAJOR = 0, 1, 2, 3
CLS = {
    'motorway': MAJOR, 'trunk': MAJOR, 'primary': MAJOR,
    'secondary': ROAD, 'tertiary': ROAD,
    'unclassified': MINOR, 'residential': MINOR,
}
TOL_M = {MAJOR: 18.0, ROAD: 22.0, MINOR: 40.0, WATER: 30.0}
# Residential stubs (cul-de-sacs, driveways) are pure clutter at 69 m/px.
MIN_LEN_M = {MAJOR: 40.0, ROAD: 50.0, MINOR: 130.0, WATER: 120.0}

def xy(lat, lon):
    return ((lon - HOME_LON) * KM_PER_DEG * LON_SCALE * 1000.0,
            (lat - HOME_LAT) * KM_PER_DEG * 1000.0)

def dist_km(lat, lon):
    x, y = xy(lat, lon)
    return math.hypot(x, y) / 1000.0

def dp(pts, tol):
    """Douglas-Peucker on projected metres."""
    if len(pts) < 3: return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1: continue
        ax, ay = pts[a][2], pts[a][3]
        bx, by = pts[b][2], pts[b][3]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i][2], pts[i][3]
            if norm < 1e-9:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dy * px - dx * py + bx * ay - by * ax) / norm
            if d > best: best, bi = d, i
        if best > tol:
            keep[bi] = True
            stack.append((a, bi)); stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]

def length_m(pts):
    return sum(math.hypot(pts[i][2] - pts[i-1][2], pts[i][3] - pts[i-1][3])
               for i in range(1, len(pts)))

d = json.load(open('osm.json'))
ways_out = []

for e in d['elements']:
    t = e.get('tags', {})
    geom = e.get('geometry') or []
    if len(geom) < 2: continue

    hw = t.get('highway')
    if hw in CLS:
        cls = CLS[hw]
    elif t.get('aeroway') == 'runway':
        cls = MAJOR
    elif t.get('waterway') == 'river':
        cls = WATER
    elif t.get('waterway') == 'stream':
        cls = WATER
    elif t.get('natural') == 'water':
        cls = WATER
    else:
        continue

    pts = [(g['lat'], g['lon']) + xy(g['lat'], g['lon']) for g in geom]

    # Streams and ponds are mostly noise at 69 m/px — keep only the big ones.
    if cls == WATER and t.get('waterway') == 'stream' and length_m(pts) < 4000:
        continue
    if cls == WATER and t.get('natural') == 'water':
        xs = [p[2] for p in pts]; ys = [p[3] for p in pts]
        if math.hypot(max(xs) - min(xs), max(ys) - min(ys)) < 500: continue

    # Clip to the scope: keep contiguous runs inside CLIP_KM, carrying one
    # point past each boundary so lines still leave the circle cleanly.
    inside = [dist_km(p[0], p[1]) <= CLIP_KM for p in pts]
    runs, cur = [], []
    for i, p in enumerate(pts):
        if inside[i]:
            if not cur and i > 0: cur.append(pts[i-1])
            cur.append(p)
        else:
            if cur:
                cur.append(p); runs.append(cur); cur = []
    if cur: runs.append(cur)

    for run in runs:
        if len(run) < 2: continue
        simp = dp(run, TOL_M[cls])
        if len(simp) < 2: continue
        if length_m(simp) < MIN_LEN_M[cls]: continue
        ways_out.append((cls, simp))

ways_out.sort(key=lambda w: w[0])   # water, minor, road, major
total = sum(len(w[1]) for w in ways_out)
import collections
c = collections.Counter(w[0] for w in ways_out)
names = {MINOR:'minor', ROAD:'road', MAJOR:'major', WATER:'water'}
for k in sorted(c): print(f'  {names[k]:6s} ways={c[k]:5d} pts={sum(len(w[1]) for w in ways_out if w[0]==k)}')
print('TOTAL ways=%d pts=%d  (flash ~%.1f KB)' % (len(ways_out), total, (total*8 + len(ways_out)*8)/1024))
json.dump([{'c': w[0], 'p': [[round(p[0],5), round(p[1],5)] for p in w[1]]} for w in ways_out],
          open('map.json','w'), separators=(',',':'))
