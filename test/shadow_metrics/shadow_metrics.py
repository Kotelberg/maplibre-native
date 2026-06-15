#!/usr/bin/env python3
"""Objective shadow-quality gates (SHADOW_REWRITE_DESIGN.md §6).

Renders synthetic, uniform-density scenes through a built `mbgl-render` (Metal) and asserts
quantitative properties of the directional cast shadows — so shadow quality is judged by NUMBERS,
not by eye (the project's recurring failure mode). Exits non-zero if any gate fails.

Usage:
    python3 test/shadow_metrics/shadow_metrics.py [path/to/mbgl-render]
    # default: build-macos-metal/bin/mbgl-render

Requires: Python 3, Pillow, numpy. Metal build of mbgl-render (the shadow path is Metal-only).

Gates:
  1. ACNE        — a pitched building's wall + roof have ~zero high-frequency energy (no self-shadow
                   stripes / moiré). Regression guard for the slope-bias + bilinear-PCF fixes.
  2. ZOOM-INVARIANT — the cast shadow's size relative to the building is constant across z16/z17/z18
                   (sun is world-fixed; shadow length must not depend on camera zoom).
  3. OFF-PATH    — MLN_RENDER_3D_ENHANCEMENTS=0 produces NO shadow (byte-identical-off guard).
"""
import json
import os
import subprocess
import sys
import tempfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("shadow_metrics: needs Pillow + numpy (pip install Pillow numpy)")

RENDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "..", "build-macos-metal", "bin", "mbgl-render")
TMP = tempfile.mkdtemp(prefix="shadow_metrics_")
FAILURES = []


def render(style, out, x, y, z, p, b=0, w=480, h=520, env=None):
    e = dict(os.environ, MLN_RENDER_3D_ENHANCEMENTS="1")
    if env:
        e.update(env)
    style_path = os.path.join(TMP, "style.json")
    json.dump(style, open(style_path, "w"))
    r = subprocess.run([RENDER, "-s", style_path, "-o", out, "-x", str(x), "-y", str(y),
                        "-z", str(z), "-p", str(p), "-b", str(b), "-w", str(w), "-h", str(h)],
                       capture_output=True, text=True, env=e)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit(f"render failed ({r.returncode}): {r.stderr[-400:]}")
    return np.asarray(Image.open(out).convert("L")).astype(float)


def light(az, polar, intensity=0.5):
    return {"anchor": "map", "intensity": 0.5, "position": [1.5, az, polar],
            "cast-shadows": True, "shadow-intensity": intensity}


def box_style(az, polar, height=60):
    return {"version": 8, "light": light(az, polar),
            "sources": {"g": {"type": "geojson", "data": {"type": "Feature", "properties": {},
                "geometry": {"type": "Polygon", "coordinates": [[[-0.0002, -0.0002], [-0.0002, 0.0002],
                    [0.0002, 0.0002], [0.0002, -0.0002], [-0.0002, -0.0002]]]}}}},
            "layers": [{"id": "bg", "type": "background", "paint": {"background-color": "#cccccc"}},
                       {"id": "b", "type": "fill-extrusion", "source": "g",
                        "paint": {"fill-extrusion-height": height, "fill-extrusion-base": 0,
                                  "fill-extrusion-color": "#dddddd"}}]}


def hf_energy(img, r0, r1, c0, c1):
    q = img[r0:r1, c0:c1]
    lap = np.abs(4 * q[1:-1, 1:-1] - q[:-2, 1:-1] - q[2:, 1:-1] - q[1:-1, :-2] - q[1:-1, 2:])
    return float(lap.mean())


def gate(name, ok, detail):
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILURES.append(name)


# Gate 1 — ACNE: pitched building, grazing-ish sun; wall + roof must be smooth.
print("Gate 1: acne (self-shadow stripes / moiré)")
a = render(box_style(300, 60), os.path.join(TMP, "acne.png"), 0.0, 0.0, 18.5, 58, b=20, w=600, h=800)
wall = hf_energy(a, 250, 410, 300, 420)
roof = hf_energy(a, 110, 150, 220, 400)
gate("acne", wall < 2.0 and roof < 2.0, f"wall_HF={wall:.2f} roof_HF={roof:.2f} (both must be <2.0)")

# Gate 2 — ZOOM-INVARIANT: shadow extent / building extent constant across zoom.
print("Gate 2: zoom-invariant shadow length")
def shadow_to_building(z):
    im = render(box_style(300, 60), os.path.join(TMP, f"zi{z}.png"), 0.0, 0.0, z, 55, b=0, w=500, h=500)
    shadow = int(((im >= 150) & (im <= 200)).sum())   # ground-shadow band
    building = int((im < 150).sum())                  # dark building faces
    return shadow / building if building else 0.0
ratios = [shadow_to_building(z) for z in (16.0, 17.0, 18.0)]
spread = (max(ratios) - min(ratios)) / (sum(ratios) / len(ratios)) if all(ratios) else 9.9
gate("zoom-invariant", spread < 0.35, f"shadow/building ratios={[round(r,3) for r in ratios]} spread={spread:.2f} (<0.35)")

# Gate 3 — OFF-PATH: turning shadows off must REMOVE the shadow. The shadow is what ON has that OFF
# lacks (ON darker than OFF); comparing isolates the cast shadow from the building's own dark faces.
print("Gate 3: off-path (MLN_RENDER_3D_ENHANCEMENTS=0 removes the shadow)")
on = render(box_style(300, 60), os.path.join(TMP, "on.png"), 0.0, 0.0, 18.0, 55, b=0)
off = render(box_style(300, 60), os.path.join(TMP, "off.png"), 0.0, 0.0, 18.0, 55, b=0,
             env={"MLN_RENDER_3D_ENHANCEMENTS": "0"})
shadow_px = int(((on + 6.0) < off).sum())   # ground/surfaces ON darkens that OFF leaves lit = the shadow
# OFF must not itself darken anything ON leaves lit (no stray shadow when disabled).
stray_off = int(((off + 6.0) < on).sum())
gate("off-path", shadow_px > 500 and stray_off < 200,
     f"shadow present only with ON: on-darker px={shadow_px} (>500), off-darker px={stray_off} (<200)")

print()
if FAILURES:
    sys.exit(f"SHADOW METRICS FAILED: {', '.join(FAILURES)}")
print("ALL SHADOW METRICS PASSED")
