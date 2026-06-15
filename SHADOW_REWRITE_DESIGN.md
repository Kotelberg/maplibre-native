# Light-Owned Directional Shadows — Design Spec

**Branch:** `shadows/s1-shadowmap-ios` → new work  ·  **Backend:** Metal first (GL/VK parity later)
**Status:** DESIGN — for review before implementation
**Companion:** see `SHADOW_ROOT_CAUSE.md` for why the current per-layer approach is being replaced.

---

## 1. Goal & non-goals

**Goal.** Directional building/model shadows that match the user's spec and Mapbox's shape:
- World-anchored: a shadow is fixed in world space; it does **not** move when the camera rotates or
  tilts. Buildings cast onto the ground **and** onto each other (and onto models / from models).
- Owned by the **style light**, not by an individual layer. One shared shadow texture for the frame.
- Correct for HataHub's **real** style: multiple fill-extrusion layers (`building-3d`, `building-hover`,
  `building-listings`, `building-selected`) plus runtime highlight/model layers.
- Visually correct **and objectively verified** — coverage even across the pitched view, shadows
  aligned to their casters, no near-field over-darkening, no far-field dropout.

**Non-goals (for v1).** Soft/area shadows, time-of-day sun, terrain shadow receivers, GL/Vulkan
parity (Metal first; keep the off-path byte-identical). Cascaded shadow maps are **optional** (see §7).

---

## 2. Why the current architecture is wrong (root cause of the rewrite)

`RenderFillExtrusionLayer` each own a `ShadowMap` + caster pass + ground-shadow layer-group +
`worldToLightClip`. Consequences:
- **Wrong render graph for multi-layer styles.** HataHub has ≥4 fill-extrusion layers; each would
  spin up its own shadow map / ground quads / caster pass, with no shared notion of "the scene's
  shadows" and fragile draw-order between them, the basemap, highlights and models.
- **No single insertion point.** Mapbox draws the ground shadow once, at `shadow-draw-before-layer`.
  Per-layer ownership can't express that.
- **Models can't participate.** A fill-extrusion layer can't collect model-layer casters.
- It "passes" single-layer headless/unit tests while still being structurally unable to produce a
  correct composite for the real style. (Confirmed: the single-layer demo still renders wrong, so
  there is *also* a rendering-quality bug to fix here — ownership alone is not the whole fix.)

Mapbox public API confirms the target shape: `DirectionalLight` owns `cast-shadows`,
`shadow-intensity`, `shadow-draw-before-layer`; `FillExtrusionLayer` only opts in via
`fill-extrusion-cast-shadows`; `ModelLayer` has `model-cast-shadows` / `model-receive-shadows`.

---

## 3. Target architecture

```
RenderOrchestrator
 ├─ ShadowPass  (NEW, renderer-owned, one per frame)        ← owns the shadow system
 │   ├─ light state: sunDir (anchor:map), intensity, color, enabled, drawBeforeLayer
 │   ├─ worldToLightClip  (one frustum/frame; padding-aware; bearing-invariant)
 │   ├─ ShadowMap  (one depth-capable RenderTarget)
 │   └─ caster collection: all shadow-casting 3D drawables from ALL layers → one caster pass
 ├─ layer groups (basemap …)
 │   └─ [ground-shadow draw inserted here, ONCE, at drawBeforeLayer]
 ├─ fill-extrusion layers (receive: sample shared shadow texture)
 ├─ model layers (cast + receive)
 └─ highlight / runtime layers
```

### 3.1 Light ownership (style + evaluated state)
- Extend `LightProperties` (`src/mbgl/style/light_impl.hpp`, currently
  `Properties<LightAnchor, LightPosition, LightColor, LightIntensity>`) with:
  `LightCastShadows` (bool, default per style), `LightShadowIntensity` (float), `LightShadowColor`
  (color), and `LightShadowDrawBeforeLayer` (string layer id, optional). Wire through
  `light.hpp`/`light_impl.cpp`/`render_light.*` like the existing properties.
- Sun direction = existing `LightPosition` + `LightAnchor`. **Require `anchor:"map"`** for a
  world-fixed sun (the live R2 style is `viewport`; see §8 deploy).
- This makes shadows a property of the *scene's light*, evaluated once, available to the ShadowPass
  and to every receiver — not recomputed per layer.

### 3.2 The ShadowPass (renderer-owned)
- New type owned by `RenderOrchestrator`, created when the evaluated light has `cast-shadows: true`
  and the backend is Metal. Holds the single `ShadowMap` (move `shadow_map.*` ownership here) and the
  per-frame `ShadowFrustumState`.
- Rendered where render targets render today — `renderer_impl.cpp:350`, in the opaque pass, before
  the main layer groups — so the shadow texture is ready before any receiver draws.
- **Frustum**: move `computeWorldToLightClip` here. Keep the world-anchored, bearing-invariant design
  (symmetric radius about the look-at), **but feed it the real camera** including `CAMERA_PADDING`
  (center on the actual look-at, not the geometric screen center). This is where the coverage/quality
  must be *proven*, not assumed (objective metric in §6).

### 3.3 Caster collection across layers
- The orchestrator (or the ShadowPass) gathers caster drawables from every layer that opts in:
  - fill-extrusion layers with `fill-extrusion-cast-shadows` → their building geometry;
  - model layers with `model-cast-shadows` → their model geometry.
- Mechanism options (decide in review, see §9):
  - **(a) Shared caster layer-group** owned by the ShadowPass; each participating render layer adds
    its caster drawables to it (smallest change to the existing per-layer drawable build).
  - **(b) Pass queries layers** for a `getShadowCasters()` drawable list each frame.
- All casters render into the one shared shadow map with the one shared `worldToLightClip`
  (depth-tested, nearest-to-light wins — the existing depth attachment already works).

### 3.4 Ground shadow — drawn once
- A single ground-shadow receiver (full-tile `z=0` quads or a screen-space pass) sampling the shared
  shadow texture, inserted **once** at `shadow-draw-before-layer` (default: just above the basemap /
  below buildings). Depth-tested against the scene so buildings occlude it correctly.

### 3.5 Receivers
- Fill-extrusion (and model) layers that receive shadows sample the **shared** shadow texture with the
  **shared** `worldToLightClip` (supplied by the ShadowPass, not a per-layer copy). The existing
  receiver shaders (`fill_extrusion_shadow.hpp`, the `ndc.z∈[0,1]` guard) largely carry over.

---

## 4. Rendering-quality fixes (must be solved IN this rewrite — not just ownership)

The single-layer demo proves ownership isn't the whole story. The rewrite must also fix:
1. **Coverage feed**: frustum centered on the real (padding-aware) look-at; covers the visible
   pitched field; objective near/far parity (§6).
2. **Sun**: world-fixed (anchor:map); elevation chosen so shadows read clearly without being
   horizon-long; consistent with the fill-extrusion face shading (same sun vector — already shared).
3. **Intensity balance**: near field must not crush to black; far field must remain visible. Separate
   "shadow darkness" from the buildings' own away-face directional shading (which currently
   compounds the near-field darkness).
4. **Edge fade** in **world** units (or none), never pitch-gated.

---

## 5. File-level change map (initial)

- `src/mbgl/style/light_impl.hpp`, `include/mbgl/style/light.hpp`, `src/mbgl/style/light.cpp`,
  `src/mbgl/style/conversion/light.cpp` — add shadow light properties + parsing.
- `src/mbgl/renderer/render_light.*` — evaluate + expose shadow light state.
- `src/mbgl/renderer/shadow_pass.{hpp,cpp}` (NEW) — the renderer-owned pass (owns ShadowMap +
  ShadowFrustumState + frustum + caster pass + ground draw).
- `src/mbgl/renderer/render_orchestrator.*`, `renderer_impl.cpp` — own + drive the ShadowPass.
- `src/mbgl/renderer/shadows/*` — `computeWorldToLightClip` moves into the pass; keep `shadow_frustum`
  + `shadow_sun` pure libs + tests.
- `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` — **remove** per-layer ShadowMap / caster
  / ground-shadow ownership; instead register casters + opt into receiving.
- `render_model_layer` (if present) — caster + receiver opt-in.
- Shaders unchanged in spirit (caster `shadow_depth`, receivers); ground receiver consolidated.

---

## 6. Objective verification (MANDATORY — my visual judgment has been unreliable)

No "looks good" sign-off. Acceptance is by numbers + diffs:
1. **Band-coverage metric** (extend the `/tmp` Python probe into a checked-in tool): on the
   viz/normal render, shadow/ground ratio across N horizontal bands must be within a tolerance of
   each other (no bottom-heavy / far-dropout). Run at z15/z16, p45/p55/p60.
2. **Analytic alignment test**: synthetic building of known height/footprint + known sun → assert the
   rendered shadow tip lands at `P + H·(S.xy/S.z)` (pixel assertion), and stays put across pitch and
   bearing.
3. **Rotation stability**: render at bearing 0/90/200, assert the shadow on a fixed building does not
   move (pixel diff under threshold).
4. **Reference images**: commit known-good PNGs; CI/dev diffs against them.
5. **On-device build stamp**: a visible version string on the HataHub map screen so we never again
   debug a binary we can't confirm is live.
6. **Off-path**: `MLN_RENDER_3D_ENHANCEMENTS=0` stays byte-identical; non-Metal builds compile.

---

## 7. Performance & cascades

- One caster pass + one shadow texture + one sample per receiver fragment = Mapbox-class cost.
- Single shadow map has a near/far resolution-vs-coverage tradeoff at steep pitch. If §6 shows the
  far field is too soft with one map, add a **second cascade** (Mapbox uses 2): near cascade
  high-res/small, far cascade low-res/large; receivers pick by view depth. Decide from the metric,
  not preemptively.

---

## 8. HataHub integration & deploy

1. Rebuild the framework (automated) **and** add the on-screen build stamp (§6.5) so "is the new
   engine live?" is never a guess again.
2. Deploy the staged `light.staged.json` (`anchor:"map"` + shadow light props) to **production R2**
   and bump `MAP_STYLE_VERSION` 7→8. *Production change — explicit go-ahead required.*
3. Feed `CAMERA_PADDING` into the camera the ShadowPass reads (already part of the camera state; the
   frustum must use the padded look-at).

---

## 9. Open questions for review

1. **Caster collection mechanism**: shared caster layer-group (§3.3a) vs pass-queries-layers (§3.3b)?
   (Leaning 3.3a — least disruptive to the existing drawable build.)
2. **Style-spec surface**: add the four light shadow properties to the MapLibre style spec now, or
   keep them fork-local (env/style-extension) until upstreaming is considered? (Fork-only per project
   constraints — likely fork-local, parsed leniently.)
3. **Ground receiver**: per-tile `z=0` quads (current) vs a single screen-space ground pass? (Quads
   are simpler and depth-test naturally; screen-space is fewer draws.)
4. **Cascades**: commit to 2 cascades up front (more code, best quality) or single-map-then-measure
   (§7)? (Leaning single-map-then-measure.)
5. **Scope of "receivers"**: ground + buildings for v1; add model-receive in v1 or v2?

---

## 10. Phasing (each phase independently verifiable)

- **P1 — Light ownership**: add shadow light properties + evaluated state; no behavior change yet.
- **P2 — ShadowPass skeleton**: renderer-owned pass owning the single ShadowMap + frustum; FE layer
  registers casters to it; receivers sample the shared texture. Remove per-layer shadow ownership.
  Single-FE-layer parity with today (verified by §6 metrics, not eyes).
- **P3 — Multi-layer + ground-once**: caster collection across all FE (+ model) layers; one ground
  draw at `shadow-draw-before-layer`. Verify on HataHub's real multi-layer style headless.
- **P4 — Quality**: padding-aware frustum, sun/intensity/length tuning, edge fade; pass all §6 metrics
  at z15/16 × p45/55/60 × bearing 0/90/200.
- **P5 — Integration**: build stamp, on-device verification, anchor:map R2 deploy (with go-ahead).
- **P6 — (optional) cascades** if §6 shows far-field softness.
