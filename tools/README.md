# Basemap generation

`src/roads_data.h` is generated, not hand-edited.

    curl -s https://overpass-api.de/api/interpreter --data-urlencode data@q.overpass -o osm.json
    python3 gen.py     # clip to 16.5 km, Douglas-Peucker, classify -> map.json
    python3 emit.py    # -> src/roads_data.h

Home is 51.0447, -114.0719 (the configured centre); it is hardcoded in both scripts and
must match `HOME_LAT` / `HOME_LON` in `src/config.h`. Simplification tolerance is
18-30 m by class against a scope that draws ~69 m per pixel, so it is sub-pixel.

Map data (c) OpenStreetMap contributors, ODbL.
