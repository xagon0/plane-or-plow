#!/usr/bin/env python3
"""Configure Plane or Plow for a location and build its basemap.

    python3 tools/configure.py --address "Cochrane, Alberta" --ssid MyWiFi --password hunter2
    python3 tools/configure.py --lat 51.0447 --lon -114.0719 --radius 20

Writes two files, both gitignored:

    src/secrets.h      WiFi credentials, scope centre, radius, timezone
    src/roads_data.h   OpenStreetMap geometry around that centre

Re-run it any time you move the scope, change the radius, or want fresher map
data. Only --address or --lat/--lon is required; credentials can be supplied
later by editing src/secrets.h.

Map data (c) OpenStreetMap contributors, ODbL.
"""

import argparse
import json
import math
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UA = "plane-or-plow-configure/1.0"
OVERPASS = "https://overpass-api.de/api/interpreter"
NOMINATIM = "https://nominatim.openstreetmap.org/search"

# Render classes. These values ARE the MAP_* enum written into roads_data.h,
# and ascending order is also the painter's order: water underneath, highways
# on top. Defining them once, here, is deliberate — when the generator and the
# firmware disagreed about these numbers, every class rendered as its
# neighbour and residential streets came out in the river's colour.
WATER, MINOR, ROAD, MAJOR = 0, 1, 2, 3
CLASS_NAME = {WATER: "water", MINOR: "minor", ROAD: "road", MAJOR: "major"}

HIGHWAY_CLASS = {
    "motorway": MAJOR, "trunk": MAJOR, "primary": MAJOR,
    "secondary": ROAD, "tertiary": ROAD,
    "unclassified": MINOR, "residential": MINOR,
}

# Simplification tolerance and minimum feature length, per class, in metres.
# The scope draws roughly 69 m per pixel at a 15 km radius, so these are all
# sub-pixel; residential stubs below the minimum are cul-de-sac clutter.
TOLERANCE_M = {MAJOR: 18.0, ROAD: 22.0, MINOR: 40.0, WATER: 30.0}
MIN_LENGTH_M = {MAJOR: 40.0, ROAD: 50.0, MINOR: 130.0, WATER: 120.0}

# Alberta has been on UTC-6 year round since 2026, so a bare offset is correct
# here and a DST rule would put the clock an hour out every winter. Regions that
# do still switch need the full rule, e.g. "MST7MDT,M3.2.0,M11.1.0".
DEFAULT_TZ = "MDT6"

MIN_STREAM_M = 4000.0     # shorter creeks are noise at this scale
MIN_LAKE_M = 500.0        # ponds below this read as specks


def http_get(url, data=None, timeout=180):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def geocode(address):
    q = urllib.parse.urlencode({"q": address, "format": "json", "limit": 1})
    hits = json.loads(http_get(f"{NOMINATIM}?{q}", timeout=45))
    if not hits:
        sys.exit(f"Could not geocode {address!r}. Try a more specific address, "
                 f"or pass --lat/--lon directly.")
    h = hits[0]
    return float(h["lat"]), float(h["lon"]), h.get("display_name", address)


class Projection:
    """Local equirectangular projection in metres about the scope centre."""

    def __init__(self, lat, lon):
        self.lat, self.lon = lat, lon
        self.lon_scale = math.cos(math.radians(lat))

    def xy(self, lat, lon):
        return ((lon - self.lon) * 111320.0 * self.lon_scale,
                (lat - self.lat) * 111320.0)

    def dist_km(self, lat, lon):
        x, y = self.xy(lat, lon)
        return math.hypot(x, y) / 1000.0


def douglas_peucker(pts, tol):
    """pts: [(lat, lon, x, y), ...]. Returns the simplified subset."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        ax, ay = pts[a][2], pts[a][3]
        bx, by = pts[b][2], pts[b][3]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        best, best_i = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i][2], pts[i][3]
            if norm < 1e-9:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dy * px - dx * py + bx * ay - by * ax) / norm
            if d > best:
                best, best_i = d, i
        if best > tol:
            keep[best_i] = True
            stack.append((a, best_i))
            stack.append((best_i, b))
    return [p for p, k in zip(pts, keep) if k]


def polyline_length_m(pts):
    return sum(math.hypot(pts[i][2] - pts[i - 1][2], pts[i][3] - pts[i - 1][3])
               for i in range(1, len(pts)))


def fetch_osm(lat, lon, clip_km):
    r = int(clip_km * 1000)
    query = f"""
[out:json][timeout:180];
(
  way(around:{r},{lat},{lon})["highway"~"^(motorway|trunk|primary|secondary|tertiary|unclassified|residential)$"];
  way(around:{r},{lat},{lon})["waterway"~"^(river|stream)$"];
  way(around:{r},{lat},{lon})["natural"="water"];
  way(around:{r},{lat},{lon})["aeroway"="runway"];
);
out geom;
"""
    print(f"  querying Overpass for {clip_km:.1f} km around {lat:.5f}, {lon:.5f} ...")
    for attempt in range(3):
        try:
            return json.loads(http_get(OVERPASS, data=query.encode(), timeout=240))
        except Exception as e:                                   # noqa: BLE001
            if attempt == 2:
                sys.exit(f"Overpass request failed: {e}\n"
                         f"The public endpoint is rate limited and returns 504s "
                         f"under load; wait a minute and try again.")
            print(f"  attempt {attempt + 1} failed ({e}); retrying in 20 s ...")
            time.sleep(20)


def classify(tags):
    hw = tags.get("highway")
    if hw in HIGHWAY_CLASS:
        return HIGHWAY_CLASS[hw]
    if tags.get("aeroway") == "runway":
        return MAJOR
    if tags.get("waterway") in ("river", "stream"):
        return WATER
    if tags.get("natural") == "water":
        return WATER
    return None


def build_ways(elements, proj, clip_km):
    out = []
    for el in elements:
        tags = el.get("tags", {})
        geom = el.get("geometry") or []
        if len(geom) < 2:
            continue
        cls = classify(tags)
        if cls is None:
            continue

        pts = [(g["lat"], g["lon"]) + proj.xy(g["lat"], g["lon"]) for g in geom]

        if cls == WATER:
            if tags.get("waterway") == "stream" and polyline_length_m(pts) < MIN_STREAM_M:
                continue
            if tags.get("natural") == "water":
                xs = [p[2] for p in pts]
                ys = [p[3] for p in pts]
                if math.hypot(max(xs) - min(xs), max(ys) - min(ys)) < MIN_LAKE_M:
                    continue

        # Keep contiguous runs inside the clip radius, carrying one point past
        # each boundary so features still leave the circle cleanly.
        inside = [proj.dist_km(p[0], p[1]) <= clip_km for p in pts]
        runs, cur = [], []
        for i, p in enumerate(pts):
            if inside[i]:
                if not cur and i > 0:
                    cur.append(pts[i - 1])
                cur.append(p)
            elif cur:
                cur.append(p)
                runs.append(cur)
                cur = []
        if cur:
            runs.append(cur)

        for run in runs:
            if len(run) < 2:
                continue
            simp = douglas_peucker(run, TOLERANCE_M[cls])
            if len(simp) < 2 or polyline_length_m(simp) < MIN_LENGTH_M[cls]:
                continue
            out.append((cls, simp))

    out.sort(key=lambda w: w[0])      # water, minor, road, major
    return out


def write_roads_header(ways, lat, lon, clip_km, path):
    points, rows = [], []
    for cls, pts in ways:
        rows.append((len(points), len(pts), cls))
        points.extend((p[0], p[1]) for p in pts)

    lines = [
        "#ifndef ROADS_DATA_H",
        "#define ROADS_DATA_H",
        "",
        "#include <Arduino.h>",
        "",
        "// " + "-" * 73,
        "// GENERATED FILE - do not edit. Rebuild with:",
        "//     python3 tools/configure.py",
        "//",
        f"// Map geometry within {clip_km:.1f} km of the configured centre.",
        "// Simplified with Douglas-Peucker at 18-40 m depending on class; the",
        "// scope draws roughly 69 m per pixel, so that is sub-pixel.",
        "//",
        "// Stored as one flat point pool plus a way index: thousands of separate",
        "// C arrays would cost more in per-array overhead than the geometry.",
        "//",
        "// Map data (c) OpenStreetMap contributors, ODbL.",
        "// " + "-" * 73,
        "",
        "enum {",
        "    MAP_WATER = 0,   // rivers, major creeks, lakes",
        "    MAP_MINOR = 1,   // residential and unclassified roads",
        "    MAP_ROAD  = 2,   // secondary and tertiary",
        "    MAP_MAJOR = 3    // motorway, trunk, primary, runways",
        "};",
        "",
        "struct MapPt  { float lat, lon; };",
        "struct MapWay { uint32_t start; uint16_t count; uint8_t cls; };",
        "",
        "static const MapPt map_pts[] PROGMEM = {",
    ]

    line = "   "
    for la, lo in points:
        tok = " {%.5ff,%.5ff}," % (la, lo)
        if len(line) + len(tok) > 96:
            lines.append(line)
            line = "   "
        line += tok
    if line.strip():
        lines.append(line)
    lines += ["};", "", "static const MapWay map_ways[] PROGMEM = {"]

    line = "   "
    for start, count, cls in rows:
        tok = " {%d,%d,%d}," % (start, count, cls)
        if len(line) + len(tok) > 96:
            lines.append(line)
            line = "   "
        line += tok
    if line.strip():
        lines.append(line)
    lines += [
        "};",
        "",
        "static const int MAP_WAY_COUNT = sizeof(map_ways) / sizeof(MapWay);",
        "static const int MAP_PT_COUNT  = sizeof(map_pts)  / sizeof(MapPt);",
        "",
        "#endif // ROADS_DATA_H",
        "",
    ]
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return len(points), len(rows)


def write_secrets(path, ssid, password, name, lat, lon, radius, tz):
    """Preserve anything already set that the caller did not override."""
    cur = open(path).read() if os.path.exists(path) else ""

    def existing(key, default):
        m = re.search(r'#define\s+%s\s+"([^"]*)"' % key, cur)
        return m.group(1) if m else default

    ssid = ssid if ssid is not None else existing("WIFI_SSID", "your-network")
    password = password if password is not None else existing("WIFI_PASS", "your-password")
    tz = tz if tz is not None else existing("HOME_TZ", DEFAULT_TZ)
    name = name if name is not None else existing("HOME_NAME", "Somewhere")

    with open(path, "w") as f:
        f.write(f'''#ifndef SECRETS_H
#define SECRETS_H

// ---------------------------------------------------------------------------
// GENERATED by tools/configure.py. NOT tracked by git - keep it that way.
// ---------------------------------------------------------------------------

#define WIFI_SSID   "{ssid}"
#define WIFI_PASS   "{password}"

#define HOME_NAME   "{name}"
#define HOME_LAT    {lat!r}
#define HOME_LON    {lon!r}
#define RADIUS_KM   {radius}

// Timezone in POSIX TZ form, used for the backlight schedule.
#define HOME_TZ     "{tz}"

#endif // SECRETS_H
''')
    return ssid


def main():
    ap = argparse.ArgumentParser(
        description="Configure Plane or Plow for a location and build its basemap.")
    loc = ap.add_argument_group("location (one of)")
    loc.add_argument("--address", help='e.g. "Cochrane, Alberta"')
    loc.add_argument("--lat", type=float)
    loc.add_argument("--lon", type=float)
    ap.add_argument("--name", help="label for the location; defaults to the address")
    ap.add_argument("--radius", type=float, default=15.0,
                    help="scope radius in km (default 15)")
    ap.add_argument("--ssid", help="WiFi SSID")
    ap.add_argument("--password", help="WiFi password")
    ap.add_argument("--tz", help='POSIX timezone. Include the DST rule only if '
                                 'your region observes DST: "MDT6" (Alberta), '
                                 '"PST8PDT,M3.2.0,M11.1.0" (US Pacific)')
    ap.add_argument("--skip-map", action="store_true",
                    help="rewrite secrets.h only; leave the basemap alone")
    args = ap.parse_args()

    secrets_path = os.path.join(ROOT, "src", "secrets.h")

    if args.address:
        lat, lon, display = geocode(args.address)
        print(f"Geocoded {args.address!r}\n  -> {lat:.5f}, {lon:.5f}  ({display})")
    elif args.lat is not None and args.lon is not None:
        lat, lon, display = args.lat, args.lon, f"{args.lat:.5f}, {args.lon:.5f}"
    else:
        if not os.path.exists(secrets_path):
            ap.error("give --address, or --lat and --lon")
        cur = open(secrets_path).read()
        lat = float(re.search(r"#define\s+HOME_LAT\s+(\S+)", cur).group(1))
        lon = float(re.search(r"#define\s+HOME_LON\s+(\S+)", cur).group(1))
        display = "existing secrets.h"
        print(f"Reusing centre from secrets.h: {lat:.5f}, {lon:.5f}")

    # Only override the stored label if we actually learned a new one.
    name = args.name or args.address
    if name and len(name) > 40:
        name = name.split(",")[0][:40]

    ssid = write_secrets(secrets_path, args.ssid, args.password, name,
                         lat, lon, args.radius, args.tz)
    print(f"Wrote src/secrets.h  (SSID {ssid!r}, radius {args.radius:g} km)")
    if ssid == "your-network":
        print("  ! No SSID set yet - edit src/secrets.h or re-run with --ssid/--password")

    if args.skip_map:
        print("Skipping basemap (--skip-map).")
        return

    # Fetch slightly beyond the scope so features leave the circle cleanly.
    clip_km = args.radius * 1.1
    proj = Projection(lat, lon)
    data = fetch_osm(lat, lon, clip_km)
    print(f"  {len(data['elements'])} ways returned")

    ways = build_ways(data["elements"], proj, clip_km)
    if not ways:
        sys.exit("No map features found near that location - check the coordinates.")

    counts = {}
    for cls, pts in ways:
        c = counts.setdefault(cls, [0, 0])
        c[0] += 1
        c[1] += len(pts)
    for cls in sorted(counts):
        n, p = counts[cls]
        print(f"  {CLASS_NAME[cls]:6s} ways={n:5d} points={p}")

    path = os.path.join(ROOT, "src", "roads_data.h")
    npts, nways = write_roads_header(ways, lat, lon, clip_km, path)
    print(f"Wrote src/roads_data.h  ({npts} points, {nways} ways, "
          f"~{(npts * 8 + nways * 8) / 1024:.0f} KB flash)")
    print("\nNext:  pio run -t upload")


if __name__ == "__main__":
    main()
