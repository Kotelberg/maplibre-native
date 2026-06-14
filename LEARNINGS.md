# Learnings

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
