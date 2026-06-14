# Learnings

## Metal ground-shadow steep-pitch far cut-off — view-depth fade (the right far-fade)

- What we learned: at steep pitch (~p60) on the device's bottom-heavy CAMERA_PADDING ({top88,right24,bottom130,left24}) the ground cast shadows showed a hard horizontal line — full in the near/bottom half, none in the far/top half. The old UV-radial fade (`r = max(|uv.x-0.5|,|uv.y-0.5|)*2` over `[fade_start,1]`) is SYMMETRIC about the LIGHT-frustum center, so it fades the near (bottom) shadows too and does NOT graceful-out the actual far edge where the cut-off is. The correct softener is a VIEW-DEPTH fade: fade by the ground fragment's camera distance (clip-space `w` from the regular tile→clip perspective matrix), so near=full, far=graceful, regardless of WHERE on screen that is. Critical under padding: the look-at is pushed up-screen, so the minimum-depth (closest) ground is mid-screen, NOT the screen bottom — a naive screen-Y fade is wrong; view-w is right.
- Key empirical facts (headless `mbgl-render` + new `MLN_RENDER_PADDING` harness, p60 393x852 device-inset, viz mode): refViewW = `getCameraToCenterDistance() * worldPerPixel` (worldPerPixel measured by projecting two adjacent center pixels) — and refViewW == the distCap radius (both are `cameraToCenter * wpp`). The visible ground's view_w is NON-monotonic top→bottom under padding: highest at the far top (~2.0x ref), minimum mid-screen (~0.5x ref), rising again at the very bottom (~0.6x ref). Calibrated defaults: fade start=1.3x ref (keeps the entire near/mid incl. the mid peak full), end=2.6x ref (fully faded by the far frustum edge). Near/mid bands stay byte-identical to no-fade; far bands taper smoothly.
- Why it matters: Phase-1 proved the STARK device line is LIVE-ONLY (streaming/inconsistent per-frame tilesets + dynamic camera under padding); a static jumpTo — even with exact insets and pixelRatio 3 — shows only a mild graceful far thinning, not a hard line. So the robust fix is not "fix the static repro" (nothing to fix there) but a mechanism that turns ANY far deficit (frustum edge, caster-tile horizon, padding miscentering, live misregistration) into a guaranteed smooth taper. The view-depth fade does that and is conservative (dormant where there's no far shadow).
- Padding-aware focal centering was NOT added: the repro shows blue≈0 (full frustum coverage) everywhere and the far deficit is GREEN (in-frustum, genuine no-caster), so re-centering the frustum would only redistribute texel density, not fix coverage — and the prior `projectedCenter*tileSize_D` attempt was a proven 512× regression. Leave `computeWorldToLightClip` focal unchanged.
- How to apply it: ground vertex outputs `clip.w` as a `view_w` varying; fragment multiplies the shadow alpha by `1 - smoothstep(depth_fade_start, depth_fade_end, view_w)`. Bounds computed CPU-side in `GroundShadowTweaker` (`computeDepthFadeBounds`) as multiples of refViewW, pitch-gated (≥30°, default) so low/moderate pitch is untouched. `GroundShadowPropsUBO` grew 2→3*16 bytes (added `depth_fade_start`/`depth_fade_end`+2 pad). Knobs: `MLN_SHADOW_DEPTH_FADE_START`(1.3), `_END`(2.6), `_PITCH`(30); `_END<=_START` or `<=0` disables. The building receiver (`fill_extrusion_shadow.hpp`) was left alone — the symptom and fix are ground-only, and the inter-building shadows are verified-good. Off path (`MLN_RENDER_3D_ENHANCEMENTS=0`) byte-identical (warm md5 `d55b595...`).
- Harness: `MLN_RENDER_PADDING="top,right,bottom,left"` added to `bin/render.cpp` (sscanf → `EdgeInsets(t,l,b,r)` → `CameraOptions::withPadding`; gated, off = byte-identical) lets headless reproduce the device's asymmetric look-at shift. Verify per-band on the viz red→green mix (red now ramps to green as the shadow fades, so band red% reads EFFECTIVE post-fade shadow).

## Metal ground-shadow frustum registration

- What we learned: the shadow caster, building receiver, and ground receiver must use one shared `worldToLightClip` per rendered frame. Computing it independently per `TileLayerGroup` is fragile because the groups can contain different drawables while tiles stream in; the caster group also lived outside normal fill-extrusion cleanup, so stale caster drawables could survive tile-cover changes.
- Why it matters: ground-shadow viz can show full frustum coverage while red caster coverage is sparse. In that state the ground quad is sampling valid UVs, but not necessarily the same shadow-map registration the caster used, or not a caster set that matches the current visible tiles.
- How to apply it: keep frustum fitting camera-state-only in `src/mbgl/renderer/shadows/shadow_tweakers.cpp`, cache it in `ShadowFrustumState` by `PaintParameters::frameCount`, and pass the same state into `ShadowDepthTweaker`, `FillExtrusionShadowTweaker`, and `GroundShadowTweaker`. In `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp`, prune `shadowMap->casterGroup()` anywhere building/ground drawables are pruned, and recreate a tile when required shadow sidecar drawables are missing.

## Metal ground-shadow caster cull (THE fix — supersedes the height-scale theory below)

- What we learned: the broken ground shadows (8× over-sized detached blobs at z16/p35, then thin road-tracing acne after the height-scale change) were caused by the shadow **caster** using FRONT-face culling. The "second-depth / render-back-faces" trick assumes a CLOSED mesh, but fill-extrusion buildings are OPEN (walls + roof, no floor). Front-cull drops the roof — the actual nearest-to-light occluder — leaving only thin far-wall slivers, so the shadow map has almost no solid content. Switching the caster to render FRONT faces (cull BACK = roof + sun-facing walls, the true occluder) produces solid, correctly-sized, attached cast shadows. Verified headless: top-down z17/p0 shadow mask ≈ 1.4× footprint area (vs 6.9× with the front-cull bug), shadow length ≈ building height at the style's 45° sun, buildings stay acne-free (receiver `shadow_bias` handles the mild self-shadow risk).
- Why it matters: this is a geometry/topology issue, not a scale issue. With the roof casting, `matrixFor`'s native z-scale of 1 is already correct/isotropic (the FE height vertex is in the same world units as the x/y footprint), so NO ground-specific height rescale is needed.
- How to apply it: in `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` set the shadow caster `CullFaceMode` to `side = Back` (render front faces). `MLN_SHADOW_CULL` (0/1/2) is kept as a debug knob. Height scale stays at `matrixFor` default (`MLN_SHADOW_ZSCALE` defaults to 1.0, tunable only for on-device length dialing).
- SUPERSEDED: the "height scale" entry below (commit c763b70) shrank the symptom by making buildings nearly flat in light space — that produced thin acne lines, not solid shadows. It was a wrong diagnosis (cull was the real cause). Kept for history only.

## Metal ground-shadow height scale (SUPERSEDED — see caster-cull entry above)

- What we learned: the ground-shadow light path must scale fill-extrusion Z by the drawable's `OverscaledTileID::overscaledZ` tile scale when `MLN_GROUND_SHADOWS` is enabled. In the Kyiv z17 render, vector data is z14 canonical but the drawable represents z17, so using `tileID.toUnwrapped().canonical.z` gives `0.5` and only halves the oversized shadows; using `overscaledZ` gives `512 / 8192 = 0.0625`, reducing the top-down shadow mask from ~21.3k px to ~2.8k px.
- Why it matters: `toUnwrapped()` intentionally drops overzoom information for tile positioning, but the shadow height scale needs render-zoom units. Mixing canonical source zoom with render zoom leaves ground shadows over-scaled.
- CAVEAT: this only shrank the over-sized symptom into thin acne lines; the real cause was the caster front-face cull (see above). The z-scale is now back to 1.0.

## Metal ground-shadow caster experiments (REVISED conclusion)

- What we learned: the earlier note here claimed cull changes don't help and only the height scale does — that conclusion was WRONG. The earlier "detached blocks from front-face depth" observation was front-face casting combined with the still-unfixed (over-large) height, never tested together with the correct z-scale. Full front-face casting (roof + near walls, cull BACK) WITH z-scale 1.0 gives correct solid shadows. (Top-cap-only WOULD detach — the gap from base to the offset roof shadow — because only the roof casts; full front faces include the near walls that fill that gap.)
- How to apply it: render front faces (cull back). See the caster-cull entry above.

## macOS env unset ordering for mbgl-render

- What we learned: on macOS, `env -u NAME` must appear before environment assignments. `env MLN_RENDER_3D_ENHANCEMENTS=1 -u MLN_GROUND_SHADOWS ...` fails with `env: -u: No such file or directory`.
- Why it matters: the ground-shadow gate checks `std::getenv("MLN_GROUND_SHADOWS")`, so setting it to `0` still enables the path, and failed unset commands can silently invalidate render comparisons.
- How to apply it: use commands like `env -u MLN_GROUND_SHADOWS MLN_RENDER_3D_ENHANCEMENTS=1 ./build-macos-metal/bin/mbgl-render ...` for building-receiver-only comparisons.

## Metal shadow focal UV alignment

- What we learned: for the z16 Kyiv test view, `Projection::project(state.getLatLng(LatLng::Unwrapped), state.getScale())` and a center-pixel point derived through `screenCoordinateToTileCoordinate(...)->matrixFor(...)->tileCornerToWorld(...)` land in the same `matrixFor` world space; the remaining center UV offset came from `ShadowFrustum::heightExpand()` adding caster height into the light-space X/Y AABB.
- Why it matters: a height-expanded light-space AABB shifts the ground focal point by half of the projected max-height vector, so the screen-center ground point can read away from `(0.5, 0.5)` even when the ground footprint is centered on the right world point.
- How to apply it: in `src/mbgl/renderer/shadows/shadow_tweakers.cpp`, derive the focal point through the tile-local `matrixFor` path, then offset the fitted footprint center by the inverse light-space projection of `0.5 * MLN_SHADOW_MAX_HEIGHT`. When verifying UV with `include/mbgl/shaders/mtl/ground_shadow.hpp`, use an opaque debug return or CPU `MLN_SHADOW_DBG` focal UV logs because non-premultiplied UV debug RGB is alpha-blended over the style color.

## Metal shadow frustum depth mapping

- What we learned: `matrix::ortho()` uses GL-style near/far depth semantics and is not safe for projecting an arbitrary light-space AABB directly into a Metal shadow map.
- Why it matters: passing `box.min.z`/`box.max.z` as near/far can put caster geometry outside Metal's `[0, w]` clip range, leaving the shadow target white while the receiver still compares invalid depths.
- How to apply it: in `src/mbgl/renderer/shadows/shadow_frustum.cpp`, map the light-space AABB directly to Metal clip space: X/Y to `[-1, 1]`, Z to `[0, 1]`. Verify with forced caster outputs:
  `cmake --build build-macos-metal --target mbgl-render -j8` and the warm `mbgl-render` shadow command from the task.

## Metal FE ground-shadow receiver wiring

- What we learned: a ground shadow receiver in `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` needs its own `LayerTweaker`; `FillExtrusionShadowTweaker` expects FE paint binders and skips drawables without them.
- Why it matters: the receiver quad can share the FE tile/light matrices, but it cannot reuse the FE drawable UBO path directly.
- How to apply it: build the z=0 tile quad as a low-priority, depth-disabled drawable in the dedicated ground-shadow tile group, bind the existing shadow map texture, and let `GroundShadowTweaker` populate `GroundShadowDrawableUBO` and `GroundShadowPropsUBO`.

## Metal FE ground-shadow layer-group isolation

- What we learned: Metal fill-extrusion buildings and ground-shadow receiver quads must not share the same `TileLayerGroup`; even a binder-less receiver quad in the building group can change the group-wide 3D depth/stencil path and flatten extrusions.
- Why it matters: `src/mbgl/mtl/tile_layer_group.cpp` derives depth/stencil handling from all enabled 3D drawables in a group, while the main framebuffer renders equal layer indexes in activation order.
- How to apply it: create a dedicated ground-shadow tile group in `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp`, attach `GroundShadowTweaker` to that group only, activate/reindex it before the building group, and add receiver drawables there with `MLN_GROUND_SHADOWS=1`.
