# Light-Owned Directional Shadows — Design Spec

**Branch:** `shadows/s1-shadowmap-ios` → new work  ·  **Backend:** Metal first (GL/VK parity later)
**Status:** DESIGN — reviewed (open questions §9 resolved); ready to implement P1.
**Companion:** see `SHADOW_ROOT_CAUSE.md` for why the current per-layer approach is being replaced.

> **Review note (2026-06-15).** Direction confirmed (light-owned, one shadow map, one caster pass,
> one ground insertion point). Seven draft flaws fixed below: ground-receiver depth semantics (§3.4),
> style surface is **fork-local legacy `light`**, not Mapbox-JSON-compatible (§3.1), render-**target**
> pass timing (§3.2), model scope = **v2** (§1, §3.5), caster **registry** keyed by `{layerID,tileID}`
> (§3.3), synthetic-first acceptance metric (§6), and the 2-cascade claim marked `[Unverified]` (§7).

---

## 1. Goal & non-goals

**Goal.** Directional building shadows that match the user's spec and Mapbox's render *shape*:
- World-anchored: a shadow is fixed in world space; it does **not** move when the camera rotates or
  tilts. Buildings cast onto the ground **and** onto each other.
- Owned by the **style light** (renderer-owned pass), not by an individual layer. One shared shadow
  texture for the frame.
- Correct for HataHub's **real** style: multiple fill-extrusion layers (`building-3d`, `building-hover`,
  `building-listings`, `building-selected`) plus runtime highlight layers.
- Visually correct **and objectively verified** — coverage even across the pitched view, shadows
  aligned to their casters, no near-field over-darkening, no far-field dropout.

**v1 scope:** ground + fill-extrusion **cast & receive**. **Model shadows are v2** — the local
`ModelLayer` is a thin fork extension (model-id/scale/rotation/opacity/footprint, contact-shadows only;
`render_model_layer.cpp`), so model *receive* would pull material/shader scope into the rewrite. The
`ShadowPass` is designed so model *casting* can be added without restructuring, but neither model cast
nor receive is in v1 acceptance.

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
 ├─ model layers (cast: v2-ready hook; receive: v2)
 └─ highlight / runtime layers
```

### 3.1 Light ownership (style + evaluated state) — FORK-LOCAL legacy `light`
- **Surface caveat:** this extends MapLibre's **legacy root `light`** object, which is a *fork-local
  compatibility surface* — it is **not** Mapbox-JSON-compatible. Mapbox's public shape is a `lights`
  array of typed entries with nested properties:
  `lights: [{ "id": ..., "type": "directional", "properties": { "cast-shadows": true,
  "shadow-intensity": ..., "direction": [...] } }]`
  (`mapbox-maps-ios .../DirectionalLight.swift`). We deliberately do **not** mutate the MapLibre style
  spec; we only add lenient parser + evaluated-state support so the HataHub style can carry the props.
- Extend `LightProperties` (`src/mbgl/style/light_impl.hpp`, currently
  `Properties<LightAnchor, LightPosition, LightColor, LightIntensity>`) with:
  `LightCastShadows` (bool), `LightShadowIntensity` (float), `LightShadowColor` (color), and
  `LightShadowDrawBeforeLayer` (string layer id, optional). Wire through
  `light.hpp`/`light_impl.cpp`/`conversion/light.cpp`/`render_light.*` like the existing properties,
  parsed leniently (absent ⇒ defaults).
- Sun direction = existing `LightPosition` + `LightAnchor`. **Require `anchor:"map"`** for a
  world-fixed sun (the live R2 style is `viewport`; see §8 deploy).
- This makes shadows a property of the *scene's light*, evaluated once, available to the ShadowPass
  and to every receiver — not recomputed per layer.

### 3.2 The ShadowPass (renderer-owned)
- New type owned by `RenderOrchestrator`, created when the evaluated light has `cast-shadows: true`
  and the backend is Metal. Holds the single `ShadowMap` (move `shadow_map.*` ownership here) and the
  per-frame `ShadowFrustumState`.
- **Render timing — the render-*target* pass, not "the opaque pass".** Render targets run as their own
  pass: `renderer_impl.cpp` uploads them (`visitRenderTargets(...upload)`, ~`:289`) then renders them
  (`renderTarget.render(...)`, ~`:350`) **before the main framebuffer clear and before the main
  opaque/translucent layer-group passes** (~`:384`/`:398`). `RenderTarget::render` itself runs its own
  internal opaque **and** translucent sub-passes against the offscreen texture (`render_target.cpp:96`).
  So the ShadowPass: (1) registers its `RenderTarget` with the orchestrator (`AddRenderTargetRequest`,
  as today); (2) builds/uploads caster drawables in the upload phase; (3) the caster pass executes
  inside the render-target pass, leaving the shadow texture ready before any receiver draws. Spell the
  upload/change timing out in the implementation so tile streaming doesn't leave a frame with an
  empty map.
- **Frustum**: move `computeWorldToLightClip` here. Keep the world-anchored, bearing-invariant design
  (symmetric radius about the look-at), **but feed it the real camera** including `CAMERA_PADDING`
  (center on the actual look-at, not the geometric screen center). This is where the coverage/quality
  must be *proven*, not assumed (objective metric in §6).

### 3.3 Caster collection across layers
- **Mechanism (resolved, §9.1): a ShadowPass-owned caster *registry*, keyed by `{layerID, tileID}`.**
  Each participating render layer builds shadow sidecar (caster) drawables during its normal
  `update()` and registers/removes them in the registry by `{layerID, tileID}`. The pass renders the
  whole registry into the shared shadow map.
  - **Not** a single raw shared `TileLayerGroup`: today's removal is `removeDrawables(pass, tileID)`
    (`render_fill_extrusion_layer.cpp:116`), which is keyed by tile only. With multiple layers sharing
    one group that is too coarse — layer A's tile removal would evict layer B's casters, or stale
    casters from a removed layer/tile would linger. The registry must distinguish layer id.
  - **Not** pass-queries-`getShadowCasters()` each frame: keeps the build in the layers' existing
    update path; the pass only owns lifecycle + the render.
- Participating layers: fill-extrusion (`fill-extrusion-cast-shadows`, default true) for v1; model
  layers (`model-cast-shadows`) register through the same path in v2.
- All casters render into the one shared shadow map with the one shared `worldToLightClip`
  (depth-tested, nearest-to-light wins — the existing depth attachment already works).

### 3.4 Ground shadow — drawn once (draw-order occlusion, NOT scene-depth test)
- A single ground-shadow receiver — **per-tile `z=0` quads** (resolved §9.3; reuses tile/matrix
  machinery, avoids reconstructing ground in screen space) — sampling the shared shadow texture,
  inserted **once** at `shadow-draw-before-layer` (default: just above the basemap, below buildings).
- **Occlusion is by DRAW ORDER, not by depth-testing against already-rendered buildings.** Buildings
  are drawn *after* the ground shadow and are opaque, so they overwrite the ground-shadow pixels where
  a building stands — that is the occlusion. The ground quads therefore do **not** rely on building
  depth being present in the buffer when they draw (it isn't yet — see the render-order constraint and
  today's `setEnableDepth(false)` at `render_fill_extrusion_layer.cpp:621`). The earlier draft's
  "depth-tested against the scene" was wrong for this order.
- *If* true per-fragment ground occlusion by buildings is ever needed (e.g. a building's far base edge
  occluding ground behind it independent of draw order), it requires an explicit **scene-depth
  prepass** the ground quad can test against — called out here as a deliberate, separate option, not
  assumed for v1.

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
  / ground-shadow ownership; instead register caster drawables in the ShadowPass registry (by
  `{layerID, tileID}`) + opt into receiving the shared texture.
- `render_model_layer.cpp` — **v2**: register model casters via the same registry hook (no v1 change).
- Shaders unchanged in spirit (caster `shadow_depth`, receivers); ground receiver consolidated.

---

## 6. Objective verification (MANDATORY — my visual judgment has been unreliable)

No "looks good" sign-off. Acceptance is by numbers + diffs:
1. **Band-coverage metric** (extend the `/tmp` Python probe into a checked-in tool): shadow/ground
   ratio across N horizontal bands must be within tolerance of each other (no bottom-heavy /
   far-dropout). **Acceptance gate runs on SYNTHETIC, uniform-density scenes** (a regular grid of
   equal-height buildings) so the metric measures the *engine*, not real-world building density —
   which skews the ratio and was part of why earlier eyeballing misled. Real HataHub views are kept
   as **regression images** (diffed, not thresholded on the band ratio). Run at z15/z16, p45/p55/p60.
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
  far field is too soft with one map, add a **second cascade** (cascaded shadow maps are the standard
  technique for large outdoor scenes; *`[Unverified]` whether Mapbox specifically uses exactly 2 — its
  renderer is in the closed binary core, not the local `mapbox-maps-ios` checkout*): near cascade
  high-res/small, far cascade low-res/large; receivers pick by view depth. **Decision (§9.4):
  single-map-then-measure** — ship one map, keep `ShadowPass` conceptually cascade-capable, and let
  the §6 metrics force the second cascade rather than committing up front.

---

## 8. HataHub integration & deploy

1. Rebuild the framework (automated) **and** add the on-screen build stamp (§6.5) so "is the new
   engine live?" is never a guess again.
2. Deploy the staged `light.staged.json` (`anchor:"map"` + shadow light props) to **production R2**
   and bump `MAP_STYLE_VERSION` 7→8. *Production change — explicit go-ahead required.*
3. Feed `CAMERA_PADDING` into the camera the ShadowPass reads (already part of the camera state; the
   frustum must use the padded look-at).

---

## 9. Decisions (resolved in review 2026-06-15)

1. **Caster collection** → **ShadowPass-owned caster registry keyed by `{layerID, tileID}`** (§3.3).
   Layers build sidecar caster drawables in their normal `update()` and register/remove by
   `{layerID, tileID}`. Not a raw shared `TileLayerGroup` (tile-only removal is too coarse for
   multiple layers); not pass-queries-layers.
2. **Cascades** → **single-map-then-measure** (§7). Keep `ShadowPass` cascade-capable; ship one map;
   let §6 metrics force a second cascade if needed.
3. **Ground receiver** → **per-tile `z=0` quads**, **draw-order composited below buildings** (§3.4) —
   not depth-tested against already-rendered buildings unless a scene-depth prepass is added.
4. **Model shadows** → **v2** (§1). v1 = ground + fill-extrusion cast/receive. Model *casting* may be
   added earlier via the registry hook, but neither model cast nor receive is in v1 acceptance.
5. **Style-spec** → **fork-local legacy `light`** (§3.1). Add parser + evaluated-state support only;
   do not mutate the public MapLibre style spec. Name it clearly as fork-local; Mapbox's public shape
   is the `lights: [{ type: "directional", properties: ... }]` array, not root `light`.

---

## 10. Phasing (each phase independently verifiable)

- **P1 — Light ownership**: add shadow light properties + evaluated state; no behavior change yet.
- **P2 — ShadowPass skeleton**: renderer-owned pass owning the single ShadowMap + frustum; FE layer
  registers casters to it; receivers sample the shared texture. Remove per-layer shadow ownership.
  Single-FE-layer parity with today (verified by §6 metrics, not eyes).
- **P3 — Multi-layer + ground-once**: caster registry collects across all **fill-extrusion** layers
  (`building-3d` + hover/listing/selected), keyed `{layerID, tileID}`; one ground draw at
  `shadow-draw-before-layer`. Verify on HataHub's real multi-layer style headless. *(Model casters are
  v2 via the same registry hook.)*
- **P4 — Quality**: padding-aware frustum, sun/intensity/length tuning, edge fade; pass all §6 metrics
  on synthetic scenes at z15/16 × p45/55/60 × bearing 0/90/200, + HataHub regression images.
- **P5 — Integration**: on-screen build stamp, on-device verification, anchor:map R2 deploy (with go-ahead).
- **P6 — (optional) cascades** if §6 shows far-field softness.
- **v2 (later)** — model-layer cast + receive (needs model-layer style props + material/shader work).

---

## 11. Implementation log (2026-06-15, branch `shadows/s1-shadowmap-ios`)

**P1, P2, P3 DONE + committed (fork-only, NOT pushed); verified headless via `mbgl-render`.**

- **P2** (`be08bbeae524`) — renderer-owned `ShadowPass` (`renderer/shadows/shadow_pass.{hpp,cpp}`)
  owns the single `ShadowMap` (RenderTarget + depth + packed-depth texture) + the per-frame
  `ShadowFrustumState`. `RenderOrchestrator::updateLayers` ensures it under the Metal+`shadowsEnabled`
  gate, registers its RenderTarget once, and hands it to each fill-extrusion layer (identified by
  static type-info — RTTI is off) via `setShadowPass()`. FE layers no longer own a ShadowMap or the
  target lifecycle. `shadowsEnabled()`/`shadowMapSize()` moved to `shadow_pass.{hpp,cpp}` as the
  single source of truth. Verified: 1FE parity, off-path creates no pass.
- **P3** (`a8dfce6c88fa`) — caster *registry* keyed by layer id (`casterGroupFor`): one
  `TileLayerGroup` per FE layer, all added to the one shared shadow RenderTarget (reusing the map's
  built-in group for the first); each layer prunes only its own tiles (no cross-eviction). **Ground-
  once**: the orchestrator marks the lowest FE layer as the single ground-shadow owner; only it draws
  the z=0 ground quads. Verified on a 2-FE synthetic scene: **ONE** shadow map for two FE layers (one
  `MLN_SHADOW_DUMP`, was N), and a 2nd identical FE layer → pixel-identical output (max diff 0) — so
  casters add nothing for shared geometry and ground draws exactly once.
- **P1** (`bda2f5612f69`) — fork-local legacy-`light` props: `cast-shadows` (bool, default **true** =
  prior behavior) gates the pass (env kill-switch AND evaluated `cast-shadows`); `shadow-intensity`
  (float, default **0.32** = approved "subtle") read by the receivers from the evaluated light
  (`MLN_SHADOW_INTENSITY` still overrides). Sun was already light-owned (LightPosition/Anchor). Wired
  through the generated light boilerplate; evaluation automatic via the LightProperties tuple.
  Verified: default unchanged (139.6), `cast-shadows:false` → no shadow + no map (204 bg),
  `shadow-intensity:0.6` → darker (83.1). Light + shadow + depth unit tests green.
- **Non-Metal guard** (`ac24ff9330d3`) — orchestrator `shadowPass` member gated behind
  `MLN_RENDER_BACKEND_METAL` (incomplete-type unique_ptr dtor otherwise).

**Deferred:** shadow-color + shadow-draw-before-layer light props (YAGNI). **Remaining:** P4 (padding-
aware frustum + checked-in band-coverage metric + regression images), P5 (framework rebuild + on-screen
build stamp + on-device verify + **production** R2 `anchor:map` deploy — needs explicit go-ahead), and
full non-Metal build verification (S1·T9 — needs a GL/Linux build config; the shadow .cpp are built
unconditionally and are backend-agnostic).

### P6 — Cascaded Shadow Maps (2026-06-15, NOT yet committed at time of writing)

**Goal:** fix artifact **A** (coarse/zoom-growing wall shadows — needs higher near-field texel density)
and artifact **B** (pitched far-field shadow dropout — needs far coverage) at once, which a single map
cannot do. Approved by the user: **2 cascades** (env-tunable), **hard transition** (no cross-cascade
blend). Plan: `docs/csm-cascaded-shadows-plan.md`.

**Design — bearing-invariant CONCENTRIC cascades (NOT view-frustum split).** Every cascade shares the
same world-anchored `focalCenter` + sun-ground axes and differs only in RADIUS: cascade `count-1` = the
full far radius (today's single map, byte-identical), nearer cascades shrink by `MLN_SHADOW_CASCADE_SPLIT`
(default 0.4) per step, with NO minRadius floor (the near cascade is deliberately tighter than the
screen — that is the resolution win; fragments outside it fall back to the far cascade). Because the only
per-cascade parameter is a scalar radius, every cascade is bearing-invariant exactly like the single map.

**Storage:** N separate `RenderTarget`+`Texture2D` pairs (the gfx/mtl layer has no texture-array render
targets: `mtl/texture2d.hpp` uses `texture2DDescriptor`, `RenderPassDescriptor` binds one color+one
depth). `kMaxShadowCascades=4` sizes the UBO matrix arrays + shader varyings; `MLN_SHADOW_CASCADE_COUNT`
(default 2, clamp [1,4]) picks the live count. **count==1 reproduces the legacy single map exactly.**

**Caster:** the same caster geometry is rendered once per cascade (`shadowCasterGroups[c]` +
`ShadowDepthTweaker(cascadeIndex=c)` using `cascades[c]`); each cascade's ortho box clips out-of-range
casters. **Receiver (hard transition):** the vertex emits a light-clip position per cascade (flattened
to `shadow_pos0..3` — MSL forbids array members in a vertex-out struct); the fragment walks cascades
near→far and uses the TIGHTEST one whose light-clip `uv∈[0,1] && ndc.z∈[0,1]`, PCF-samples that cascade's
texture (`array<texture2d,4> [[texture(0)]]`), and stops. The ground receiver ties its UV-radial rim fade
to the FAR cascade's edge (the outer coverage boundary) so the near cascade's inner edge never fades
shadows mid-screen.

**Verified (headless `mbgl-render`, dense 72-building grid, z17.5/p55):** MSL cascade shaders compile +
render with no Metal errors; 2-cascade vs 1-cascade differ on ~9% of pixels (crisper near shadow edges,
max Δ69) with NO seam / missing-shadow / regression; off-path (`MLN_RENDER_3D_ENHANCEMENTS=0`) renders
unchanged; 14 shadow unit tests green incl. new `CascadesAreConcentricAndBearingInvariant` (count==1 ==
legacy byte-for-byte; far cascade unchanged when a near is added; near provably tighter; per-cascade
bearing-invariant). Env knobs: `MLN_SHADOW_CASCADE_COUNT` (2), `MLN_SHADOW_CASCADE_SPLIT` (0.4).

**Remaining:** on-device A/B validation + near-split tuning at the real Kyiv style (P5; the dramatic
improvement only shows at the production camera), framework rebuild, and the deferred R2 deploy.
