# Ground Cast Shadows — Debug Summary & Open Problem

Fork: `~/Work/maplibre-native`, branch `shadows/s1-shadowmap-ios`, **Metal backend only**.
Goal: Mapbox-v3-style **directional cast shadows** for `fill-extrusion` buildings (buildings
cast shadows onto the ground and onto each other), with a world-fixed sun.

This doc is the context for research/brainstorm subagents. It states what was built, what
works, and the **one unsolved problem** with measurable evidence.

---

## Architecture (current)

Three render passes, all gated behind `MLN_RENDER_3D_ENHANCEMENTS=1`; ground pass additionally
behind `MLN_GROUND_SHADOWS` (dev gate). Metal-only (`#if MLN_RENDER_BACKEND_METAL`).

1. **Caster depth pass** — `ShadowDepthShader` (`include/mbgl/shaders/mtl/shadow_depth.hpp`).
   Renders the fill-extrusion building geometry (`bucket.sharedTriangles`, z=base..height) from
   the **sun's POV** into a `ShadowMap` (`src/mbgl/renderer/shadows/shadow_map.cpp`): an
   RGBA8 color (packed depth) + Float32 depth `RenderTarget`, `mapSize` default 2048, cleared to
   white (= packed-far) + depth 1.0. Caster cull = **back** (render FRONT faces = roof + sun
   walls; this was a fix — see "Resolved #1"). Wiring: `render_fill_extrusion_layer.cpp` ~line 600.

2. **Building receiver** — `FillExtrusionShadowShader`
   (`include/mbgl/shaders/mtl/fill_extrusion_shadow.hpp`). The normal FE building draw, plus a
   light-space varying; fragment PCF-samples the shadow map and darkens lit faces. WORKS.

3. **Ground receiver** — `GroundShadowShader` (`include/mbgl/shaders/mtl/ground_shadow.hpp`).
   A per-tile **flat full-tile quad** (`addQuad(0,0,EXTENT,EXTENT)`, z=0, `enableDepth=false`,
   alpha-blended, drawPriority −1, drawn under the buildings). Fragment samples the shadow map at
   the ground point's light-space UV and darkens where occluded. **This is the problem pass.**

The light frustum is computed in `src/mbgl/renderer/shadows/shadow_tweakers.cpp`
`computeWorldToLightClip(layerGroup, parameters, mapSize)`:
- sun direction from style `light` (anchor `map`, position [1.5,210,45] = 45° elevation), via `ShadowSun`.
- focal center = `centerPixelToWorld(state, focalZoom)` (screen-center pixel → world via `matrixFor`),
  then `heightCompensatedFootprintCenter` (offsets by the half-maxHeight light-space projection,
  to keep the light-UV centered ~0.5 — was an alignment fix).
- radius = **viewport-derived** (latest): project a 6×6 grid of screen pixels to world, take the
  max half-extent (clamped to `MLN_SHADOW_RADIUS_MAX`=4000). [History: was fixed 700 → adaptive
  max(tileExtent, screenExtent) → viewport-only.]
- `ShadowFrustum::fit` (`shadow_frustum.cpp`): light-view (look from sun), ortho box, texel-snap
  (anti-crawl, needs world-fixed sun), Z remapped to Metal [0,1].
- **Each of the 3 tweakers calls `computeWorldToLightClip` independently, on its OWN layer group.**

Per-tile `light_matrix = worldToLightClip * matrixFor(tileID)` (z-scale 1; the height vertex is in
mercator-px units, consistent with x/y). Caster, building-receiver, ground all use this.

App side (`apps/mobile/components/listings/listing-map-screen.tsx`): MapLibre RN MapView with
`CAMERA_PADDING = {bottom:130, left:24, right:24, top:88}` (asymmetric), auto-pitch to ~50° at
zoom≥15 (gesture-triggered), an in-app style override forcing `light.intensity=0.5` + `anchor=map`.

---

## What WORKS (verified)

- Building self/cross shadows + crisp directional face shading (Mapbox-like). Acne-free.
- `MLN_RENDER_3D_ENHANCEMENTS=0` path unchanged (shadows off).
- **Frustum COVERAGE is full**: the "viz" debug (ground shader paints coverage colors — see below)
  shows **0–2% out-of-frustum ("blue")** across top/mid/bottom in every tested view (z14–16, 2D & 3D,
  dense & sparse). So the ground quad's light-space UV is in-bounds everywhere on screen.

## ✅ RESOLVED (2026-06-14): the half-screen cutoff is FIXED

Root cause = **caster/ground/receiver light-frustum MISREGISTRATION**: `computeWorldToLightClip` ran
independently per `TileLayerGroup`, so the caster wrote depth into frustum A while the ground sampled
frustum B → shadows survived only where A∩B overlapped (near camera) = the cutoff. Live≠headless
because headless renders one static, consistent tile set.

**Fix = commit `768b4853` "Share shadow frustum across passes"** (P0): one shared per-frame light
frustum from camera STATE ONLY, cached in `ShadowFrustumState` by `frameCount`, passed into all 3
tweakers; **+P1** caster group pruned with the tile lifecycle. **On-device verified (iPhone 17 sim):**
viz mode → blue=0 (full coverage) + red in TOP/MID/BOT (no red=0 band), holds under motion; normal
mode (intensity 0.5) → shadows across the entire pitched view, attached to bases, no cutoff line.

**⚠️ P3 "padding-focal" (`projectedCenter * tileSize_D`) is WRONG — a 512× regression that destroys
ALL shadows.** `centerPixelToWorld` and `Projection::project(latLng,scale)` are already the same
space. Leave focalCenter unchanged.

Mild residual (headless p60: far/top red≈4% vs mid≈13%, resolution- & azimuth-independent) = expected
perspective/grazing thinning of distant shadows, NOT a bug. Original symptom below, kept for history.

### Original symptom (now fixed)

**Ground cast shadows render only in PART of the screen at dense, pitched views** — a hard boundary
("shadows only in the bottom half"), leaving dense buildings on the other side with no ground shadow.
Reported by the user across ~6 device screenshots in different dense Kyiv districts (Центр/Золоті
ворота, Солом'янка, Лівобережний) — NOT explained by parks/density.

### Debug instrument: "viz mode"
`ground_shadow.hpp` fragment, when `props.shadow_intensity > 1.5` (set `MLN_SHADOW_INTENSITY=2.0`),
paints the ground instead of shadowing it:
- **blue** = light-UV outside [0,1] (frustum doesn't cover here),
- **red**  = shadow map has a caster at this ground UV (`occl < 0.99`),
- **green**= covered, no caster (lit ground).
Observed only via on-device `simctl io booted screenshot` (logs don't surface — see Observability).

### Measurements
- **Frustum covers** (blue ≈ 0%) everywhere — coverage is NOT the problem.
- **Casters (red) are NON-UNIFORM / sparse in dense areas LIVE.** Example, dense Поділ 3D, thirds:
  `top red=28% / mid red=20% / bottom red=0%` (and 3% blue at very bottom edge). Dense buildings exist
  throughout, yet "red" (= a caster wrote depth where the ground samples) is absent in a whole band.
- **Headless `mbgl-render` (single consistent frustum) shows UNIFORM red** (~14–23% across all thirds)
  for comparable dense pitched views. **Live ≠ headless.**
- Normal mode (`MLN_SHADOW_INTENSITY=0.5`) at the same dense views: shadows concentrated in one band,
  bare dense buildings in the other = the user's "cutoff".

### Leading hypothesis (unconfirmed)
**Caster/ground frustum divergence.** The caster pass and the ground pass call
`computeWorldToLightClip` on **different layer groups** (`shadowMap->casterGroup()` vs
`groundShadowLayerGroup`). If their inputs differ at all, the caster renders depth into frustum A
while the ground samples frustum B → the ground reads a **misregistered, mostly-empty** shadow map,
showing shadows only where A∩B overlap (near the camera). The latest fix made the radius
viewport-derived (state-only, identical across groups) to remove this — but it is **not yet
confirmed** to fix the live cutoff (verification blocked, see below), and the focal/`matrixFor`
terms may still carry per-group or padding-dependent differences.

### Alternative hypotheses to consider
- The 3 passes are genuinely inconsistent in some term other than radius (focal center under
  `CAMERA_PADDING`; the height-compensated footprint offset; `focalZoom` from "first drawable").
- Depth-comparison / RGBA8 precision at the far half (though ortho light depth is linear/uniform).
- The whole "separate flat ground quad sampling a shared shadow map" approach is fragile; a different
  architecture (e.g., a single ground-shadow pass driven by the SAME frustum object as the caster;
  or Mapbox-v3's actual ground-shadow technique) may be more robust.

## Resolved issues (history — don't re-litigate)
1. **Front-face cull dropped the roof.** FE meshes are open (walls+roof, no floor); the second-depth
   "render back faces" trick left only thin far-walls. Fix = render FRONT faces (cull **back**).
   Measured: z17/p0 shadow area **6.9× footprint → ~1×**.
2. **Height z-scale.** An earlier 0.0625 z-scale over-shrank casters → thin acne. Reverted to 1.0
   (with the roof casting, matrixFor z-scale 1 is correct; height vertex is already mercator-px).
3. **`MLN_SHADOW_RADIUS=400` leftover env** forced a fixed tiny frustum and **silently overrode every
   adaptive-radius fix on-device for many iterations** ("makes no difference"). Now unset.
4. **Alignment** (shadows offset from buildings) — fixed earlier via height-compensated focal center;
   verified center-pixel light-UV = 0.50 at p0/p30/p60 headless.

## Observability constraints (why this is hard)
- `mbgl::Log` and `fprintf(stderr)` from the framework **do NOT surface** to `simctl log show` or
  Metro (the RN MapLibre host routes mbgl logs to the JS bridge). The ONLY observable channel on
  device is **rendered pixels** (screenshot) → hence "viz mode".
- **Headless `mbgl-render` ≠ live app**: no `CAMERA_PADDING`, different & non-deterministic tile
  loading, pitch clamped to 60. Headless has shown "full coverage / uniform red" while live cuts off.
- `launchctl setenv` (used to set `MLN_*` on the sim) **clears on simulator reboot** → intermittent
  "no shadows at all".
- App fragility blocks navigation: deep-link `hatahub:///rent/kvartyra/kyiv/map` triggers a
  LocationModal infinite-loop redbox; dev-tool inspector toggles on stray taps.

## Verify loop
- Headless: `MLN_RENDER_3D_ENHANCEMENTS=1 MLN_GROUND_SHADOWS=1 MLN_SHADOW_INTENSITY=<0.5 or 2.0(viz)>
  MLN_SHADOW_MAP_SIZE=2048 ./build-macos-metal/bin/mbgl-render -s /tmp/style_map.json -o out.png
  -x 30.5235 -y 50.4470 -z 15 -p 55 -w 400 -h 760 -c /tmp/uniq_$(date +%s%N).db` then read PNG.
  Build: `cmake --build build-macos-metal --target mbgl-render -j8`.
- iOS: `bazel build //platform/ios:MapLibre.dynamic --//:renderer=metal`; unzip
  `bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip`; `touch` the `MapLibre` binaries (bazel
  zeroes mtime → Xcode skips re-embed); copy the sim slice into the installed app bundle;
  codesign --force; **uninstall + reinstall** (simctl install alone does a differential install and
  keeps the old binary); relaunch. On-device verify = simctl screenshot in viz mode.

## Key files
- `src/mbgl/renderer/shadows/shadow_tweakers.cpp` (frustum + per-tile light_matrix; the 3 tweakers)
- `src/mbgl/renderer/shadows/shadow_frustum.cpp`, `shadow_map.cpp`, `shadow_sun.cpp`
- `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` (wiring; caster + ground quad build)
- `include/mbgl/shaders/mtl/{shadow_depth,fill_extrusion_shadow,ground_shadow}.hpp`
- `src/mbgl/renderer/render_target.cpp` (shadow target clear)
