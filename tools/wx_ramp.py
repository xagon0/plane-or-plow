#!/usr/bin/env python3
"""Regenerate src/wx_ramp.h from the Environment Canada GeoMet legend.

    python3 tools/wx_ramp.py

The RADAR_1KM_RSNO legend is a continuous colourbar (snow rate, 0.1-20 cm/h,
logarithmic), not 14 flat swatches, so we sample its centre column top to bottom
rather than trying to pick out discrete blocks. A pixel's position along that
bar is already a perceptually sensible intensity, which is what the firmware
needs; nearest-neighbour against the sampled ramp turns an arbitrary tile pixel
back into one. Sampling is inset past the bar's black outline at both ends.
"""
import io, urllib.request
from PIL import Image

N = 32
URL = ("https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0"
       "&request=GetLegendGraphic&layer=RADAR_1KM_RSNO"
       "&style=Radar-Snow_14colors&format=image/png")

im = Image.open(io.BytesIO(urllib.request.urlopen(URL, timeout=60).read())).convert("RGB")
W, H = im.size
x = max(range(W), key=lambda c: sum(
    1 for y in range(H) if max(im.getpixel((c, y))) - min(im.getpixel((c, y))) > 40))
ys = [y for y in range(H)
      if max(im.getpixel((x, y))) - min(im.getpixel((x, y))) > 30 or sum(im.getpixel((x, y))) < 250]
y0, y1 = min(ys) + 4, max(ys) - 4
ramp = [im.getpixel((x, int(round(y1 - (y1 - y0) * i / (N - 1))))) for i in range(N)]

out = ["#ifndef WX_RAMP_H", "#define WX_RAMP_H", "", "#include <stdint.h>", "",
       "// " + "-" * 73,
       "// Environment Canada snow-rate colour ramp, sampled from the GeoMet legend for",
       "// RADAR_1KM_RSNO / Radar-Snow_14colors. Weakest (0.1 cm/h) first, strongest",
       "// (20 cm/h) last; the scale is logarithmic in rate, so a pixel's position along",
       "// this ramp is already a perceptually sensible intensity.",
       "//",
       "// The server antialiases when it resamples to our bbox, so tiles come back with",
       "// far more colours than the palette holds. Nearest-neighbour against this ramp",
       "// is what turns an arbitrary pixel back into an intensity.",
       "//",
       "// Regenerate with tools/wx_ramp.py.",
       "// " + "-" * 73, "",
       "#define WX_RAMP_N %d" % N, "",
       "static const uint8_t wx_ramp[WX_RAMP_N][3] = {"]
for i, c in enumerate(ramp):
    out.append("    {%3d,%3d,%3d},   // %.2f" % (c[0], c[1], c[2], i / (N - 1)))
out += ["};", "", "#endif // WX_RAMP_H", ""]

open("src/wx_ramp.h", "w").write("\n".join(out))
print("wrote src/wx_ramp.h  (%s -> %s)" % (ramp[0], ramp[-1]))
