import json
ways = json.load(open('map.json'))
MINOR, ROAD, MAJOR, WATER = 0, 1, 2, 3
# Painter's order: water underneath, highways on top.
order = {WATER: 0, MINOR: 1, ROAD: 2, MAJOR: 3}
ways.sort(key=lambda w: order[w['c']])

pts, rows = [], []
for w in ways:
    start = len(pts)
    for p in w['p']:
        pts.append(p)
    rows.append((start, len(w['p']), w['c']))
assert len(pts) < 2**32

out = []
out.append('''#ifndef ROADS_DATA_H
#define ROADS_DATA_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Map geometry within %.1f km of home (51.0447, -114.0719 — the configured centre).
//
// Generated from OpenStreetMap via the Overpass API, clipped to the scope and
// simplified with Douglas-Peucker at 18-30 m depending on class. The scope
// renders about 69 m per pixel, so the simplification is sub-pixel.
//
// Stored as one flat point pool plus a way index: 2783 separate C arrays
// would cost more in per-array overhead than the geometry itself.
//
// Map data (c) OpenStreetMap contributors, ODbL.
// ---------------------------------------------------------------------------

enum {
    MAP_WATER = 0,   // Bow River, major creeks, lakes
    MAP_MINOR = 1,   // residential + unclassified (range & township roads)
    MAP_ROAD  = 2,   // secondary + tertiary
    MAP_MAJOR = 3    // motorway, trunk, primary, runways
};

struct MapPt  { float lat, lon; };
struct MapWay { uint32_t start; uint16_t count; uint8_t cls; };
''' % 16.5)

out.append('static const MapPt map_pts[] PROGMEM = {')
line = '   '
for i, (la, lo) in enumerate(pts):
    tok = ' {%.5ff,%.5ff},' % (la, lo)
    if len(line) + len(tok) > 96:
        out.append(line); line = '   '
    line += tok
if line.strip(): out.append(line)
out.append('};\n')

out.append('static const MapWay map_ways[] PROGMEM = {')
line = '   '
for (s, c, cl) in rows:
    tok = ' {%d,%d,%d},' % (s, c, cl)
    if len(line) + len(tok) > 96:
        out.append(line); line = '   '
    line += tok
if line.strip(): out.append(line)
out.append('};\n')

out.append('static const int MAP_WAY_COUNT = sizeof(map_ways) / sizeof(MapWay);')
out.append('static const int MAP_PT_COUNT  = sizeof(map_pts)  / sizeof(MapPt);\n')
out.append('#endif // ROADS_DATA_H')

dst = '/Users/ethanfrances/Desktop/Development/embedded/ESP_AirplaneOrSnowplow/src/roads_data.h'
open(dst, 'w').write('\n'.join(out) + '\n')
print('wrote roads_data.h: %d pts, %d ways' % (len(pts), len(rows)))

# preview copy, same ordering
json.dump([{'c': w['c'], 'p': w['p']} for w in ways], open('map_ordered.json','w'),
          separators=(',',':'))
print('map_ordered.json:', len(open('map_ordered.json').read()), 'bytes')
