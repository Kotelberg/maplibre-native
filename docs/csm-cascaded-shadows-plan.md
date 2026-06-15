# P6 — Cascaded Shadow Maps (CSM) Implementation Plan

> **Branch:** `shadows/s1-shadowmap-ios` (fork-only; DO NOT push; NO upstream PR). **Backend:** Metal/iOS.
> Continues `SHADOW_REWRITE_DESIGN.md` §10 "P6 — (optional) cascades". Approved 2026-06-15.

**Goal:** Replace the single shadow map with **2 concentric, bearing-invariant cascades** (env-tunable
count) so near buildings get high texel density (fixes artifact **A** — coarse/zoom-growing wall
shadows) and the pitched far field stays covered (fixes artifact **B** — far-shadow dropout).

**Architecture:**
- **N concentric cascades**, all sharing the same world-anchored `focalCenter` and sun-ground axes,
  differing only in **radius** (cascade 0 = tight/near, cascade N−1 = today's full `reach`/far). This is
  what preserves bearing-invariance — rotating the camera moves no cascade (radius is a scalar).
- **Storage: N separate `RenderTarget` + `Texture2D` pairs** (the `gfx`/`mtl` abstraction has no
  texture-array support: `mtl/texture2d.hpp` uses `texture2DDescriptor` only; `RenderPassDescriptor`
  binds one color + one depth attachment). Texture2DArray is documented future work.
- **Receiver selection: geometric containment** — fragment uses the **smallest (tightest) cascade whose
  light-clip `uv ∈ [0,1]` and `ndc.z ∈ [0,1]`**. No camera-distance/meter splits. Hard transition.
- **Caster: render the same casters once per cascade** into that cascade's target with its
  `worldToLightClip[C]`; each cascade's ortho box clips out-of-range casters automatically.

**Defaults:** `MLN_SHADOW_CASCADE_COUNT=2`, `MLN_SHADOW_CASCADE_SPLIT=0.4` (near radius = max(minRadius,
0.4×farRadius)). `COUNT=1` ⇒ behaves exactly like today (parity fallback). Off-path
(`MLN_RENDER_3D_ENHANCEMENTS=0`) stays byte-identical.

**Verification:** headless `mbgl-render` (env `MLN_RENDER_3D_ENHANCEMENTS=1`) on the dense grid
(`/tmp/densegrid.json`) + Kyiv style, z16–z18, p45/55, comparing 1-cascade vs 2-cascade vs off; plus the
existing frustum unit tests extended for N cascades.

---

## MAX_CASCADES compile constant

Shaders need a compile-time array bound. Define `MLN_SHADOW_MAX_CASCADES = 4` (covers the env-tunable
range 1..4). UBOs size their arrays to MAX; `cascade_count` says how many are live. Receiver loops run
`for (i in 0..cascade_count)`. Keeps one shader binary across all counts.

---

### Task 1: Cascade config (pure, testable)

**Files:** Modify `src/mbgl/renderer/shadows/shadow_pass.{hpp,cpp}`

- [ ] Add `uint32_t shadowCascadeCount()` — env `MLN_SHADOW_CASCADE_COUNT`, default `2`, clamp `[1,4]`.
- [ ] Add `float shadowCascadeSplit()` — env `MLN_SHADOW_CASCADE_SPLIT`, default `0.4f`, clamp `(0,1)`.
- [ ] Verify build (`ninja mbgl-core`).

### Task 2: N concentric frustum matrices (TDD)

**Files:** Modify `src/mbgl/renderer/shadows/shadow_tweakers.cpp`,
`include/mbgl/renderer/shadows/shadow_tweakers.hpp`; Test `test/renderer/shadow_frustum.test.cpp`

- [ ] Extract the radius derivation in `computeWorldToLightClip` so the far radius is computed once,
      then add `std::vector<mat4> computeWorldToLightClipCascades(state, sunDir, mapSize, count, split)`:
      for cascade C, `radius[C] = (C == count-1) ? farRadius : max(minRadius, splitFactor[C]*farRadius)`
      (for count=2: near = max(minRadius, split×far), far = far). Same `focalCenter`, same sun-ground
      axes, same `maxHeightWorld`, per-cascade footprint at `radius[C]` → `heightExpand` → `fit`.
- [ ] Keep the existing single `computeWorldToLightClip` as `cascades(...,count=1)[0]` (byte-identical).
- [ ] **Red test:** `CascadesAreConcentricAndBearingInvariant` — count=2 returns 2 matrices; the near
      cascade maps a smaller world square to clip [-1,1] than the far (transform a known world point and
      assert near clip extent > far for the same offset); rendering at bearing 0 vs 90 gives the same
      per-cascade world coverage (transform a fixed world point, assert clip xy stable). And
      `count=1 == legacy computeWorldToLightClip` exactly.
- [ ] Implement; run test green.

### Task 3: Per-frame N-matrix cache

**Files:** Modify `include/mbgl/renderer/shadows/shadow_tweakers.hpp` (`ShadowFrustumState`),
`src/mbgl/renderer/shadows/shadow_tweakers.cpp` (`worldToLightClipForFrame`)

- [ ] `ShadowFrustumState`: replace `mat4 worldToLightClip` with `std::vector<mat4> cascades` +
      `uint32_t cascadeCount`. Keep a `const mat4& worldToLightClip()` accessor returning `cascades[0]`
      for any not-yet-migrated caller.
- [ ] `worldToLightClipForFrame` computes all cascades when the cache is stale (same frameCount/mapSize
      guard, add cascadeCount to the key).
- [ ] Build.

### Task 4: ShadowPass owns N maps

**Files:** Modify `src/mbgl/renderer/shadows/shadow_pass.{hpp,cpp}`

- [ ] `ShadowPass(mapSize, cascadeCount)`; member `std::vector<std::unique_ptr<ShadowMap>> shadowMaps_`.
- [ ] `ensure()` creates `cascadeCount_` maps. `target(i)`, `texture(i)`, `cascadeCount()` accessors.
- [ ] `casterGroupFor(context, layerID, cascadeIdx)` — registry keyed by `{layerID, cascadeIdx}`; the
      first layer reuses map[cascadeIdx]'s built-in group 0. `releaseCasterGroup(layerID)` releases all
      cascades for that layer. `clearCasters()` clears all.
- [ ] Build.

### Task 5: Orchestrator registers/​tears down N targets

**Files:** Modify `src/mbgl/renderer/render_orchestrator.{hpp,cpp}`

- [ ] Construct `ShadowPass` with `shadowCascadeCount()`; if count changed, recreate.
- [ ] Register all N `target(i)` via `AddRenderTargetRequest` (guard `shadowTargetRegistered` → make it a
      count or per-index set). On teardown (shadows inactive), `RemoveRenderTargetRequest` for each +
      `clearCasters()`.
- [ ] Pass `cascadeCount` to FE layers (extend `setShadowPass` or add `setShadowCascadeCount`).
- [ ] Build.

### Task 6: Caster pass per cascade

**Files:** Modify `src/mbgl/renderer/shadows/shadow_tweakers.cpp` (`ShadowDepthTweaker`),
`src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp`

- [ ] `ShadowDepthTweaker` takes a `cascadeIndex` (or its `frustumState` accessor returns the right
      cascade); `execute()` uses `cascades[cascadeIndex]` for `worldToLightClip`.
- [ ] FE layer: for each cascade, get `casterGroupFor(ctx, id, c)`, attach a `ShadowDepthTweaker(c)`,
      and add caster drawables (share vertex buffers; N drawable objects). Prune per `{layer,cascade}`.
- [ ] Build; one `MLN_SHADOW_DUMP` per cascade shows the near map tighter than the far.

### Task 7: Receiver UBOs + texture slots

**Files:** Modify `src/mbgl/shaders/fill_extrusion_shadow_ubo.hpp`, `src/mbgl/shaders/ground_shadow_ubo.hpp`,
`include/mbgl/shaders/shader_defines.hpp` (or wherever the receiver texture enum lives)

- [ ] `FillExtrusionShadowDrawableUBO`: `light_matrix` → `light_matrix[MLN_SHADOW_MAX_CASCADES]`; add
      `uint32_t cascade_count` (+ pad to 16). Same for `GroundShadowDrawableUBO`. Update `static_assert`
      sizes.
- [ ] Add N receiver texture slot ids (`idFillExtrusionShadowTexture0..3`, `idGroundShadowTexture0..3`)
      and bump the shaders' `textures` array sizes to `MLN_SHADOW_MAX_CASCADES`.
- [ ] Build.

### Task 8: Receiver shader cascade selection (hard transition)

**Files:** Modify `include/mbgl/shaders/mtl/fill_extrusion_shadow.hpp`,
`include/mbgl/shaders/mtl/ground_shadow.hpp`

- [ ] Vertex: emit `float4 shadow_pos[MLN_SHADOW_MAX_CASCADES]` (one per live cascade; loop to
      `cascade_count`).
- [ ] Fragment: loop cascades `0..cascade_count`; first cascade whose `uv ∈ [0,1] && ndc.z ∈ [0,1]` wins
      (tightest = highest res); PCF-sample that cascade's texture; if none contain it, fully lit. Keep
      the existing bilinear-PCF + slope-bias per chosen cascade. Hard transition (no blend).
- [ ] Build; verify shader strings via `strings <App> | grep` (no stale binary).

### Task 9: Receiver wiring binds N textures + N matrices

**Files:** Modify `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp`,
`src/mbgl/renderer/shadows/shadow_tweakers.cpp` (`FillExtrusionShadowTweaker`, `GroundShadowTweaker`)

- [ ] Receiver drawable builders: `setTexture(shadowPass->texture(i), id...Texture0+i)` for each cascade.
- [ ] Tweakers fill `light_matrix[i] = cascades[i] * tileWorld` for each cascade and set `cascade_count`.
- [ ] Build.

### Task 10: Headless verification + off-path parity

- [ ] `ninja mbgl-core mbgl-render`. Render dense grid + Kyiv style at z16/z17/z18, p45/p55, with
      `MLN_SHADOW_CASCADE_COUNT=1` vs `=2` vs `MLN_RENDER_3D_ENHANCEMENTS=0`.
- [ ] Assert: `COUNT=1` ≈ pre-CSM HEAD (parity), `COUNT=2` shows crisper near walls + far-field shadow
      where 1 dropped it; OFF byte-identical. Read PNGs to confirm.
- [ ] Run shadow unit tests (`mbgl-test-runner --gtest_filter=*Shadow*`) green.

### Task 11: Tune near split + commit

- [ ] Sweep `MLN_SHADOW_CASCADE_SPLIT` (0.3/0.4/0.5) on the dense grid; pick the value that maximizes
      near crispness without dropping near buildings out of cascade 0.
- [ ] `bun format`/`lint`/`check-types` are N/A (C++ fork) — instead ensure no `-Werror` unused vars and
      the non-Metal path still compiles (`#if MLN_RENDER_BACKEND_METAL` guards intact).
- [ ] Commit on `shadows/s1-shadowmap-ios` (NO push). Update `SHADOW_REWRITE_DESIGN.md` §11 log.
- [ ] Rebuild the dist-spm framework + demo app for on-sim reverify (artifacts NOT committed).

---

## Notes / risks
- **UBO size:** `MAX_CASCADES=4` × `float4x4` (64B) = 256B of matrices per receiver drawable UBO + count.
  Acceptable. `static_assert` must be updated or the build fails loudly (good).
- **N× caster cost:** 2× building caster draws/frame. The far cascade is the heavier (all geometry); the
  near clips most away. Mobile-acceptable; revisit with Texture2DArray + layered render only if needed.
- **Parity discipline:** keep `COUNT=1` an exact code path equal to today so any regression is bisectable.
- **Stale-binary trap:** always `ninja mbgl-core mbgl-render`; verify shader edits landed via `strings`.
