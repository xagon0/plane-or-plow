# tools

## configure.py

Sets the scope's location and builds its basemap. This is the only setup step
beyond flashing.

    python3 tools/configure.py --address "Cochrane, Alberta" --ssid Net --password pw
    python3 tools/configure.py --lat 51.0447 --lon -114.0719 --radius 20
    python3 tools/configure.py --skip-map --ssid NewNetwork --password newpw

Writes `src/secrets.h` and `src/roads_data.h`, both gitignored. Existing WiFi
credentials are preserved if you don't pass new ones.

Geometry comes from the Overpass API, clipped to 1.1x the scope radius and
simplified with Douglas-Peucker at 18-40 m by class. The scope draws roughly
69 m per pixel, so that is sub-pixel. Four classes are emitted in painter's
order — water, minor roads, secondary, highways — and those enum values are
defined once, in this script, because they have to match what the firmware
indexes its palette with.

Dense cities produce large basemaps: central Calgary at 15 km is about 44,000
points, where a town like Cochrane is about 6,500. Raise `--radius` with that
in mind.

The public Overpass endpoint is rate limited and returns 504s under load; the
script retries three times.

## wx_ramp.py

Regenerates `src/wx_ramp.h` from Environment Canada's GeoMet legend. Only
needed if EC changes the radar palette.

    python3 tools/wx_ramp.py
