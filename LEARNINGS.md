# Learnings

## Metal ground-shadow height scale

- What we learned: the ground-shadow light path must scale fill-extrusion Z by the drawable's `OverscaledTileID::overscaledZ` tile scale when `MLN_GROUND_SHADOWS` is enabled. In the Kyiv z17 render, vector data is z14 canonical but the drawable represents z17, so using `tileID.toUnwrapped().canonical.z` gives `0.5` and only halves the oversized shadows; using `overscaledZ` gives `512 / 8192 = 0.0625`, reducing the top-down shadow mask from ~21.3k px to ~2.8k px.
- Why it matters: `toUnwrapped()` intentionally drops overzoom information for tile positioning, but the shadow height scale needs render-zoom units. Mixing canonical source zoom with render zoom leaves ground shadows over-scaled.
- How to apply it: in `src/mbgl/renderer/shadows/shadow_tweakers.cpp`, compute the gated ground-shadow Z scale from `Projection::worldSize(state.getScale()) / (1 << tileID.overscaledZ) / util::EXTENT`, and keep the `env -u MLN_GROUND_SHADOWS` path at `zScale=1` for building receiver regression checks.

## Metal ground-shadow caster experiments

- What we learned: changing the shadow caster to front-face depth, or trying top-cap-only depth with culling disabled, brings back large detached ground blocks in the z16/p35 Kyiv view. Top-cap-only with the existing front-face cull removes the ground shadows entirely.
- Why it matters: the detached ground artifacts are not fixed by swapping caster culling; the useful narrow fix is the gated light-space height scale.
- How to apply it: leave the caster cull mode in `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` on the existing front-cull/back-face shadow-map path unless a future change introduces a separate ground-only shadow map.

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
