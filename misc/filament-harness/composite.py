#!/usr/bin/env python3
"""Composite the harness's raw RGBA overlay onto a map PNG.

Usage: composite.py map.png overlay.raw out.png [WxH]
"""
import sys

from PIL import Image

map_path, raw_path, out_path = sys.argv[1:4]
size = tuple(int(v) for v in (sys.argv[4] if len(sys.argv) > 4 else "800x600").split("x"))

map_img = Image.open(map_path).convert("RGBA")
with open(raw_path, "rb") as f:
    overlay = Image.frombytes("RGBA", size, f.read())

Image.alpha_composite(map_img, overlay).save(out_path)
print("wrote", out_path)
