# 3D Building Shadows — Cross-Platform Port Plan

> **Status (2026-06-16):** iOS/Metal is **shipped and in production** in the HataHub app.
> Android (GL) is **in progress**; Vulkan and Web (MapLibre GL JS) are **not started**. This
> document is the hand-off for that work, written for an engineer with **zero prior context**.

> ### ⚠️ Corrections discovered during the Android GL port (2026-06-16) — supersede claims below
>
> 1. **GL offscreen texture EXISTS** — at `src/mbgl/gl/offscreen_texture.{hpp,cpp}` (the doc looked
>    in `include/mbgl/gl/`). It was **color-only**; `Context::createOffscreenTexture(size,type,depth,
>    stencil)` *ignored* the depth flag. **✅ DONE:** depth now attaches a `DEPTH_STENCIL`
>    renderbuffer to the color-texture FBO (`gl/offscreen_texture.cpp` + new
>    `Context::createFramebuffer(Texture2D, Renderbuffer<DepthStencil>)`). The §3 "biggest blocker /
>    spike it first" framing was **overstated** — GL already had `createFramebuffer`-with-depth code.
> 2. **The C++ plumbing is NOT backend-agnostic.** §2.1's "reusable as-is" is **wrong** — it was
>    POC-grade Metal gating. The shadow *wiring* is behind `#if MLN_RENDER_BACKEND_METAL`:
>    `render_orchestrator.cpp` (4 gates: ShadowPass ctor, RenderTarget registration, `setShadowPass`/
>    ground-owner), `render_fill_extrusion_layer.cpp` (12 gates, incl. `useShadows=false` hardcoded
>    on non-Metal at L200), plus shadow refs in layer tweakers. Only the **math** is truly agnostic
>    (`renderer/shadows/*` = 0 gates). **Un-gating these is a real, sizeable task** (new task, must
>    land WITH the GL shaders so each step stays green; iOS must keep working). A backend macro
>    `#if MLN_RENDER_BACKEND_OPENGL` already exists (Goldfish mitigation, orchestrator L1033).
> 3. **No local GL verification on macOS.** Apple's GL driver rejects `#version 300 es` (GL backend
>    hardcodes it, no desktop-GLSL fallback) even in a core profile, and GLSL compiles at **runtime**
>    in the driver — so `build-gl-check` (compiles fine on macOS) validates **zero** shader code.
>    **Verify loop = physical Android device** (or emulator) via `MapLibreAndroidTestApp` **opengl**
>    flavor (builds the fork `.so` straight from this tree) + a `MapSnapshotter` instrumentation test
>    `ShadowSnapshotTest.kt` (renders the live Kyiv style at a fixed pitched camera → PNG → `adb pull`).
>    This is the on-device analog of Metal's `mbgl-render -o x.png`. **Baseline confirmed** (Kyiv 3D
>    buildings render on GL on-device, no shadows = clean stock). Commands are in `ShadowSnapshotTest.kt`.
> 4. **GL shaders have NO per-shader `.cpp`** (unlike Metal). GLSL source goes in `gl/X.hpp`
>    (`ShaderSource<…,OpenGL>` with `vertex`/`fragment` strings); attribute/UBO/texture metadata goes
>    in **`src/mbgl/shaders/gl/shader_info.cpp`** (`ShaderInfo<…,OpenGL>::{uniformBlocks,attributes,
>    textures}`). Register the 3 BuiltIn IDs in `gl/renderer_backend.cpp` `initShaders` + include the
>    3 new headers there (mirror how `mtl/renderer_backend.cpp` includes its shadow headers directly,
>    not via the generated `shader_manifest.hpp`). Add the headers to `cmake/opengl.cmake` INCLUDE_FILES.
>
> **GL port: ✅ COMPLETE + on-device verified (2026-06-16).** verify loop + baseline · offscreen
> depth target · 3 GL shaders (`gl/{shadow_depth,fill_extrusion_shadow,ground_shadow}.hpp`) +
> `shader_info.cpp` entries · plumbing un-gated via `MLN_DRAWABLE_SHADOWS`
> (`include/mbgl/renderer/shadows/shadow_support.hpp`, = Metal||OpenGL) · device A/B PROVES cast
> shadows render through GL (cast-shadows:false → clean stock; true → ground + building shadows).
> GL specifics that mattered: NO uv.y flip (GL FBO is bottom-left vs Metal top-left), `precision
> highp float` in shadow fragments (packed depth), array varyings OK, static-sampler if-chain
> (ES 3.0 forbids dynamic sampler-array indexing), unique UBO pad member names per block (no-instance
> blocks share a global member namespace → `GroundShadow{Drawable,Props}UBO` both had `u_pad0/1` =
> link error); caster must remap `gl_Position.z` `[0,1]`→`[-1,1]` (`clip.z=2z-w`, pack original z/w
> separately) or the offscreen depth test runs at half precision → roof self-shadow acne.
> **Remaining:** Vulkan (started — see below), then RN style-driven bindings.
>
> **Vulkan port: ✅ COMPLETE + on-device verified (2026-06-16, Honor/MediaTek).** 3D buildings + cast
> shadows render through the Vulkan backend (A/B: `cast-shadows:false` → clean 3D buildings, no ground
> shadows; `true` → ground + building cast shadows). **Decision = "Route B": flip Vulkan onto the
> NON-instanced fill-extrusion path** (`MLN_USE_FILL_EXTRUSION_INSTANCING` → `(0)`, the macro the fork
> already used for Metal) so the proven 3-shader architecture drops in — rather than authoring a new
> *instanced* shadow path. Route B also FIXES the instanced path's building-height "snap" during the
> z15→z16 grow and gains crease-aware facade normals; the instanced FE specializations are now
> compiled-out dead code behind the macro on every backend.
>
> What it took (net): completed the Vulkan **non-instanced** FE building shader (it was a stub —
> hardcoded `normal=(0,0,1)`, `t=1.0`; now unpacks `in ivec4 in_normal_ed` → `t=normal_ed.x&1`,
> `normal=normal_ed.xyz/16384.0`, mirroring Metal) + gated the instanced specializations/registration
> behind the macro; authored 3 Vulkan shadow shaders `vulkan/{shadow_depth,fill_extrusion_shadow,
> ground_shadow}.{hpp,cpp}` (collision/custom_geometry plain-uniform binding template; 4 discrete
> cascade samplers walked by a static if-chain; **Vulkan NDC z is [0,1] like Metal → NO GL `2z-w`
> remap**; **GL convention for the offscreen origin → NO `applySurfaceTransform` on the caster, NO
> `uv.y` flip on receivers** — caster-write and receiver-sample share the un-transformed light_matrix
> so the conventions cancel); flipped `MLN_DRAWABLE_SHADOWS` to include Vulkan; registered the 3
> shaders in `vulkan/renderer_backend.cpp` + `cmake/vulkan.cmake`.
>
> ⚠️ **THE two bugs that made buildings invisible (both Vulkan-only, neither caught by the C++
> compile — Vulkan GLSL compiles at runtime on-device):**
> 1. **Drawable-UBO off-by-one (the killer).** Vulkan binds the drawable descriptor set POSITIONALLY:
>    SSBO bindings `[0, maxSSBOCountPerDrawable)`, then UBO bindings starting AT `maxSSBOCountPerDrawable`
>    (`context.cpp::buildUniformDescriptorSetLayout` + `descriptor_set.cpp::UniformDescriptorSet::update`,
>    `setDstBinding(index)`). The fill-extrusion instance buffer was the ONLY drawable SSBO, so flipping
>    instancing off dropped `maxSSBOCountPerDrawable` 1→0 — but the GLSL prelude HARDCODED
>    `drawableUBOStartId 1` (`vulkan/common.hpp`). Result: every drawable-set UBO (the receiver's
>    **matrix**, props, the caster's matrix) bound one slot too high → read the dummy buffer → zero
>    matrix → `gl_Position = 0` → degenerate triangles → **nothing renders** (no validation error). Fix:
>    `common.hpp` now defines `drawableUBOStartId` = `MLN_VK_DRAWABLE_UBO_START` = `1` with instancing /
>    `0` without (must equal `maxSSBOCountPerDrawable`). Affects ALL drawable-set Vulkan shaders
>    (collision/custom_geometry too — they were only ever exercised with instancing on).
> 2. **Props in the wrong descriptor set.** `FillExtrusionShadowTweaker` wrote the receiver's props to
>    the LAYER group, but the Vulkan shader declares it at `DRAWABLE_UBO_SET_INDEX`; the layer set only
>    binds ids in `[layerSSBOStartId, drawableSSBOStartId)`, so the props id (drawable-range) landed in
>    an unbound slot → all-zero props (opacity/color = 0 → invisible). Metal (flat `[[buffer(id)]]`) /
>    GL (named blocks) are unaffected. Fix: write props **per-drawable on Vulkan**, keep the cheaper
>    **per-layer** write on Metal/GL (`#if MLN_RENDER_BACKEND_VULKAN` in the tweaker — props is
>    layer-constant, so per-drawable is wasted uploads on the shipped backends).
>
> **Cascade default + perf:** Vulkan exhibits the SAME 2-cascade roof over-shadow latch as GL after a
> zoom-out round-trip → defaulted Vulkan to **1 cascade** (`shadow_pass.cpp`, joined the GL gate). This
> is also the headline perf lever: Vulkan was defaulting to 2 cascades vs GL's 1 = ~2× the shadow cost
> (two caster passes + two-cascade receiver sampling); 1 cascade closes most of the GL-vs-Vulkan gap.
> Residual Vulkan cost is per-drawable descriptor updates (matrix changes every frame), inherent to
> the backend.
>
> **Net new/changed (fork, uncommitted):** new `vulkan/{shadow_depth,fill_extrusion_shadow,
> ground_shadow}.{hpp,cpp}`; `vulkan/fill_extrusion.{hpp,cpp}` (normal_ed + macro gating);
> `vulkan/common.hpp` (`MLN_VK_DRAWABLE_UBO_START`); `layer_ubo.hpp` (macro → 0); `shadow_support.hpp`
> (+Vulkan); `shadow_tweakers.cpp` (per-drawable props on Vulkan); `shadow_pass.cpp` (Vulkan→1 cascade);
> `vulkan/renderer_backend.cpp` + `cmake/vulkan.cmake` (register). Offscreen depth target was already
> done. **Open/QA:** roof over-shadow latch only spot-checked (default-1 sidesteps it); assert
> `depthReady` in `vulkan/offscreen_texture.cpp` (still silently degrades color-only); deeper perf
> profiling if 1-cascade isn't enough.
>
> **On-device verify loop (Vulkan):** `gradlew :MapLibreAndroidTestApp:installVulkanDebug` then launch
> `ShadowDemoActivity` (a live MapView → genuinely uses the Vulkan backend). Gotchas: the activity's
> INIT camera (`cameraPosition=`/`moveCamera` in `getMapAsync`) does NOT apply (surface not ready), but
> a button-driven `animateCamera(newCameraPosition(...))` (the added "GO" button) DOES — drive the
> pitched z17.5 view that way. `ShadowSnapshotTest` (deterministic z18/pitch55) is blocked by a
> pre-existing `FeatureOverviewActivity.loadFeaturesTask` NPE (`category!!` on the bundled
> ShadowDemoActivity which has no category meta) — fix `category ?: ""` to use it. Vulkan GLSL compiles
> at RUNTIME on-device, so the C++ gradle build validates only the C++ side — binding/GLSL bugs surface
> as a blank render, not a build error.

This is a fork-only feature in `Kotelberg/maplibre-native`. **Do not open upstream PRs and do
not push fork branches without explicit instruction.** All shadow code lives behind the
`MLN_RENDER_3D_ENHANCEMENTS` master gate; with the gate off the renderer is byte-identical to
stock MapLibre.

- **Branch:** `shadows/s1-shadowmap-ios` (current, not pushed)
- **Design log:** `SHADOW_REWRITE_DESIGN.md` (§11 implementation log, P4 + P6 entries)
- **Cascade design:** `docs/csm-cascaded-shadows-plan.md`

---

## 0. TL;DR for the next engineer

| Platform | State | Effort estimate | Headline blocker |
|---|---|---|---|
| **iOS / Metal** | ✅ Shipped, prod | — | none (done) |
| **Android / GL + Vulkan** | ⬜ Not started | ~3–6 weeks | GL offscreen render target with a usable **depth attachment** does not exist yet |
| **Web / MapLibre GL JS** | ⬜ Not started | ~10–13 weeks | none technical; it's a large fork + WebGL2 depth-texture plumbing |

The C++ renderer plumbing (frustum math, shadow pass orchestration, light properties, per-layer
caster wiring) is **backend-agnostic and reusable as-is** on Android. Only the **3 shaders** and
the **offscreen texture / render-target attachments** are Metal-specific and must be ported.

Web is a completely separate codebase (TypeScript / WebGL2) and gets a from-scratch
re-implementation guided by the same architecture.

---

## 1. What we built (iOS / Metal — shipped)

### 1.1 Architecture

Mapbox-v3-style directional **cast shadows** for `fill-extrusion` 3D buildings, driven by the
style's `light` object. Two render targets:

1. **Caster pass** — render every fill-extrusion building from the **sun's point of view** into
   an offscreen texture, storing each fragment's light-space depth as **RGBA8 packed depth**
   (4×8-bit pack, not a hardware depth texture — chosen for portability across backends and to
   avoid depth-texture sampling quirks).
2. **Receiver pass** — when drawing the ground and the buildings normally, re-project each
   fragment into light space and compare its depth against the caster texture (PCF-filtered). If
   the stored caster depth is nearer the sun, the fragment is in shadow → multiply its RGB by a
   shadow factor.

Two receivers exist: the **ground** (`ground_shadow`) and the **building walls/roofs**
themselves (`fill_extrusion_shadow`).

The light is **world-anchored** (`"anchor": "map"`): the sun stays at a fixed map azimuth/elevation
as you rotate/pitch the camera, so shadows are physically stable. The frustum that bounds the
caster pass is built **bearing-invariant** — it does not rotate with the camera.

### 1.2 Cascaded shadow maps (CSM)

We use **N concentric, bearing-invariant cascades** (default **2**) that share one `focalCenter`
and the same sun→ground axes, differing **only in radius**:

```
radius(cascade c) = farRadius * pow(split, count-1-c)
```

- Far cascade (`c = count-1`) = full reach (its frustum is **identical** to the legacy single-map
  frustum — this is asserted by a test, see §1.6).
- Near cascades are tighter → higher texel density near the camera → crisp contact shadows.
- `count == 1` is **byte-identical** to the pre-CSM single shadow map.
- Defaults: `MLN_SHADOW_CASCADE_COUNT=2`, `MLN_SHADOW_CASCADE_SPLIT=0.4`. Clamp `[1, 4]`,
  `kMaxShadowCascades = 4`.

**Storage:** N separate `RenderTarget` + `Texture2D` pairs. The gfx/Metal abstraction has **no
texture-array render targets** (`mtl/texture2d.hpp` uses a single `texture2DDescriptor`; a
`RenderPassDescriptor` binds exactly one color + one depth attachment), so cascades are stored as
discrete textures, not array slices. The receiver shaders bind all `kMaxShadowCascades` textures
(unused slots clamped to the far cascade's texture).

**Receiver cascade selection** (fragment shader): pick the **smallest** cascade whose light-clip
UV is in `[0,1]` and NDC z in `[0,1]` (geometric containment). The per-cascade light-space
positions are passed as **flattened** `shadow_pos0..3` varyings — MSL forbids array members
(`float4 shadow_pos[4]`) in a vertex-output struct, so they cannot be an array.

### 1.3 Key files (Metal implementation)

**Backend-agnostic renderer plumbing (reusable on Android as-is — see §2):**

| File | Role |
|---|---|
| `src/mbgl/renderer/shadows/shadow_frustum.{hpp,cpp}` | Light-space frustum / world→light-clip math |
| `src/mbgl/renderer/shadows/shadow_tweakers.cpp` | `computeWorldToLightClipCascades()` (concentric radii), `shadowHeightFade`, FE/Ground/Depth tweakers fill `light_matrix[4]` + `cascade_count` |
| `src/mbgl/renderer/shadows/shadow_pass.{hpp,cpp}` | Owns N `ShadowMap`s; `shadowCascadeCount/Split()`, `target(i)`/`texture(i)`, `casterGroupFor(layerID, cascadeIdx)`, registry keyed `{layerID, cascadeIdx}` |
| `src/mbgl/renderer/shadows/shadow_map.{hpp,cpp}` | One cascade's render target |
| `src/mbgl/renderer/shadows/shadow_sun.{hpp,cpp}` | Sun direction from light `position` |
| `src/mbgl/renderer/render_orchestrator.cpp` | Constructs `ShadowPass` with cascadeCount; registers/tears down all N targets |
| `src/mbgl/renderer/layers/render_fill_extrusion_layer.{hpp,cpp}` | `std::vector<TileLayerGroup*> shadowCasterGroups`, per-cascade caster drawables, binds `kMaxShadowCascades` textures |
| `include/mbgl/shaders/shader_defines.hpp` | `idFillExtrusionShadowTexture0..3`, `idGroundShadowTexture0..3` |
| `include/mbgl/shaders/shader_source.hpp` | `BuiltIn` enum: `ShadowDepthShader`, `FillExtrusionShadowShader`, `GroundShadowShader` (backend-agnostic — already present) |
| `include/mbgl/shaders/fill_extrusion_shadow_ubo.hpp`, `ground_shadow_ubo.hpp`, `shadow_depth_ubo.hpp` | UBO layouts (`light_matrix[4]` + `cascade_count`) |

**Metal-specific (MUST be re-authored per backend — see §2):**

| File | Role |
|---|---|
| `include/mbgl/shaders/mtl/shadow_depth.hpp` + `src/mbgl/shaders/mtl/shadow_depth.cpp` | Caster shader: writes packed-RGBA8 light-space depth |
| `include/mbgl/shaders/mtl/fill_extrusion_shadow.hpp` + `.cpp` | Building receiver: cascade selection, per-cascade bias, **wall suppression** |
| `include/mbgl/shaders/mtl/ground_shadow.hpp` + `.cpp` | Ground receiver: cascade selection, far-cascade rim fade |
| `include/mbgl/mtl/offscreen_texture.{hpp,cpp}` | Metal offscreen render target **with a depth attachment** — the GL analog **does not exist** (see §3 blocker) |

**Tests:** `test/renderer/shadow_frustum.test.cpp` — `CascadesAreConcentricAndBearingInvariant`
(count=1 ≡ legacy; far cascade unchanged; near tighter; per-cascade bearing-invariant) +
`test/renderer/shadow_sun.test.cpp`. 14 tests pass.

### 1.4 Style / light properties (what production uses)

```jsonc
"light": {
  "anchor": "map",            // world-anchored sun (NOT viewport)
  "position": [1.5, 180, 27], // [radial, azimuth°, polar°]
  "cast-shadows": true,
  "shadow-intensity": 0.12    // very light grey + transparency (user-tuned)
}
```

- `position` is `[radialCoordinate, azimuthal°, polar°]`. **MapLibre light azimuth: 0° = north**
  (under `anchor: map`), increasing **clockwise**; **polar 0° = directly overhead**.
- `[1.5, 180, 27]` = **Kyiv solar noon ~June 15**: due-south azimuth `180°`, polar `27°`
  (≈ `63°` solar elevation).
- `shadow-intensity: 0.12` = the final user-approved value ("very very light grey with a bit of
  transparency"). The shader multiplies receiver RGB by a factor derived from this; it does **not**
  tint buildings grey on its own.

### 1.5 Environment knobs (all default-off-safe)

| Var | Default | Meaning |
|---|---|---|
| `MLN_RENDER_3D_ENHANCEMENTS` | off | **Master gate.** `0` → byte-identical to stock MapLibre |
| `MLN_SHADOW_CASCADE_COUNT` | 2 | cascades, clamp `[1,4]` (`1` ≡ legacy single map) |
| `MLN_SHADOW_CASCADE_SPLIT` | 0.4 | near-cascade radius ratio |
| `MLN_SHADOW_INTENSITY` | (style) | override `shadow-intensity` |
| `MLN_SHADOW_MAP_SIZE` | 2048 | caster texture resolution |
| `MLN_SHADOW_BIAS` / `MLN_SHADOW_SLOPE_BIAS` | tuned | depth-comparison bias |
| `MLN_SHADOW_GROW_ZOOM_LO` / `_HI` | 15 / 15.2 | building grow-in ramp; **HI must match the style's `fill-extrusion-height` interpolate stop** |
| `MLN_SHADOW_TEST` | off | demo harness: `_SUN="az,polar"`, `_ZOOM`, `_PITCH`, `_LAT`, `_LON` |

### 1.6 Notable fixes & root causes (carry these forward to every backend)

1. **Wall projective-aliasing (`369b26de127b`).** Under a near-overhead sun, vertical walls are
   nearly parallel to the light, so the caster's *skyline* smears onto the wall — an **XY/projection
   error, not a depth-bias error**. Fix: classify `wallness = 1.0 - abs(normal.z)` (roof normal
   `(0,0,1)`, wall `(nx,ny,0)`) in the receiver and `lit = mix(lit, 1.0, smoothstep(0.4, 0.85, wallness))`
   to suppress cast-shadow on near-vertical surfaces. The FE bucket emits **smoothed** wall normals,
   so `normal.z` is robust; the stock `normal.y != 0` heuristic is an incomplete approximation —
   **do not use it.**
2. **Near-cascade self-shadow (`df57a3a201d4`).** The near cascade's tight frustum makes the tuned
   bias too small in light-clip space → walls self-shadow. Fix: scale depth bias `8×` on all
   cascades **except** the far one.
3. **Concentric cascades preserve bearing-invariance (`3ece644c0b82`).** First implementation floored
   near-cascade radius at `minRadius` (== far cascade), so near/far displacements were identical —
   the TDD test caught it. Fix: remove the `std::max(minRadius, …)` floor on near cascades.
4. **Grow-ramp alignment (`9cfea93e74c2`, `bfc4a7fcd6a5`).** `fill-extrusion-height`/`base`
   interpolate `15,0 → 15.2,H` (buildings reach full height shortly after z15); `shadowHeightFade`'s
   `MLN_SHADOW_GROW_ZOOM_HI` default **must** equal the style's high stop (`15.2`) or shadows
   detach from half-grown buildings.
5. **MSL array-member ban.** `float4 shadow_pos[4]` is illegal in a vertex-output struct → flatten
   to `shadow_pos0..3` + an if-chain. (GLSL/SPIR-V allow arrays in varyings — Android can keep them
   as an array if preferred.)
6. **bazel iOS needs `--//:renderer=metal`** (default `drawable`/GL tries to compile GL shadow
   shaders that don't exist yet → `gl/shadow_depth.hpp not found`).

### 1.7 How to verify (no device needed)

**Headless render** (fetches real Kyiv tiles over the network from the style's tile URLs):

```bash
cd ~/Work/maplibre-native/build-macos-metal && ninja mbgl-core mbgl-render
MLN_RENDER_3D_ENHANCEMENTS=1 ./bin/mbgl-render \
  -s /tmp/shadow_test_style.json -o /tmp/x.png \
  -z 18 -x 0.0003 -y 0.0003 -p 55 -w 512 -h 512
# then Read /tmp/x.png
```

**Tests:** `ctest` / run `shadow_frustum.test` + `shadow_sun.test` (14 pass).

**Real HataHub app on sim/device:**
```bash
cd ~/Work/hatahub/apps/mobile && CI=false npx expo run:ios --device <UDID>
# Bundle com.hatahub.app; links MapLibre via SPM local path /Users/berg/Work/maplibre-native/dist-spm
```

---

## 2. Backend-agnostic vs Metal-specific (the porting boundary)

### 2.1 Reusable **as-is** on Android (C++, no backend types)

These compile against `gfx::` abstractions and the `BuiltIn` enum — **no changes needed** for the
Android port beyond making sure they're in the build:

- `src/mbgl/renderer/shadows/*` (frustum, tweakers, pass, map, sun) — all the math + orchestration
- `src/mbgl/renderer/render_orchestrator.cpp` shadow wiring
- `src/mbgl/renderer/layers/render_fill_extrusion_layer.{hpp,cpp}` caster/receiver wiring
- `include/mbgl/shaders/shader_source.hpp` enum slots (already present)
- `include/mbgl/shaders/*_ubo.hpp` UBO layouts
- `include/mbgl/shaders/shader_defines.hpp` texture binding IDs

### 2.2 Must be **re-authored per backend** (shaders)

Three shaders, currently MSL only. Each backend needs its own `ShaderSource<BuiltIn::X, Backend::Type::Y>`
specialization:

| Shader | Metal source (reference) | Android needs |
|---|---|---|
| Caster (depth pack) | `mtl/shadow_depth.hpp` | `gl/shadow_depth.hpp`, `vulkan/shadow_depth.hpp` |
| Building receiver | `mtl/fill_extrusion_shadow.hpp` | `gl/fill_extrusion_shadow.hpp`, `vulkan/fill_extrusion_shadow.hpp` |
| Ground receiver | `mtl/ground_shadow.hpp` | `gl/ground_shadow.hpp`, `vulkan/ground_shadow.hpp` |

**Reference dual-backend shaders that already exist** (copy their structure exactly):
`include/mbgl/shaders/gl/fill_extrusion.hpp` and `include/mbgl/shaders/vulkan/fill_extrusion.hpp`.
The pattern: a templated `ShaderSource<BuiltIn::…, gfx::Backend::Type::OpenGL>` struct exposing
`name`, `vertex`, and `fragment` string literals + attribute/UBO/texture metadata.

### 2.3 Must be **created / verified** (render target with depth)

The caster pass renders into an offscreen color target (RGBA8 packed depth) **and needs a depth
attachment** so the nearest occluder wins during the caster render. Metal has
`include/mbgl/mtl/offscreen_texture.{hpp,cpp}`. **The GL backend has no
`include/mbgl/gl/offscreen_texture.*`** — see §3, this is the highest-risk Android task. The
gfx-level contract to implement against is `src/mbgl/gfx/offscreen_texture.hpp`.

> Known sharp edge (recorded in memory `project_maplibre_shadows_atmosphere.md`): on the Metal
> pipeline, enabling a depth attachment on the `RenderTarget` interfered with color writes
> (a pipeline depth-attachment gap), so reliable inter-building occlusion depends on getting the
> color+depth attachment combination right. Watch for the same when wiring GL/Vulkan render targets.

---

## 3. Android port plan (GL + Vulkan) — ordered tasks

Android ships **both** an OpenGL ES and a Vulkan backend; each needs the shaders. Do them in this
order so the build never breaks (the `shader_manifest.hpp` pulls in per-backend headers — they must
**exist** before they're included, or compilation fails).

### Phase 0 — scaffolding (build stays green)

1. Create empty/stub specialization headers so includes resolve:
   - `include/mbgl/shaders/gl/{shadow_depth,fill_extrusion_shadow,ground_shadow}.hpp`
   - `include/mbgl/shaders/vulkan/{shadow_depth,fill_extrusion_shadow,ground_shadow}.hpp`
2. Add includes to `include/mbgl/shaders/shader_manifest.hpp` (mirror how `mtl/` shadow headers are
   pulled in). The `BuiltIn` enum slots already exist — no enum changes.
3. Register the specializations in the backend shader registries:
   - GL: `src/mbgl/gl/renderer_backend.cpp` (reference: the existing fill_extrusion registration,
     ~lines 118–150)
   - Vulkan: `src/mbgl/vulkan/renderer_backend.cpp` (reference: ~lines 676–710)

### Phase 1 — caster shader (GLSL + SPIR-V/Vulkan GLSL)

4. Port `mtl/shadow_depth` → `gl/` and `vulkan/`. Vertex applies the world→light-clip matrix
   (`light_matrix[cascadeIndex]` from the UBO) and the height-grow fade; fragment **packs** the
   light-space depth into RGBA8 (port the MSL pack helper verbatim — keep the exact pack/unpack so
   the receiver's unpack matches). Mind the **GL clip-space Z range** (`[-1,1]` vs Metal/Vulkan
   `[0,1]`) — the Metal shader does a `[-1,1]→[0,1]` remap; GL must not double-remap.

### Phase 2 — receiver shaders (building + ground)

5. Port `mtl/fill_extrusion_shadow` → `gl/`, `vulkan/`. Carry **all** of: cascade selection
   (containment test), per-cascade bias scale (`8×` except far), PCF, and **wall suppression via
   `wallness`** (§1.6 #1). GLSL/SPIR-V permit array varyings, so `shadow_pos[4]` is fine — but the
   UBO `light_matrix[4]` + `cascade_count` layout must stay identical.
6. Port `mtl/ground_shadow` → `gl/`, `vulkan/` (cascade selection + far-cascade rim fade).

### Phase 3 — GL offscreen render target with depth attachment ⚠️ **BLOCKER**

7. Implement `include/mbgl/gl/offscreen_texture.{hpp,cpp}` (or extend the existing GL renderable
   path) to provide a color (RGBA8) + **depth** attachment offscreen target satisfying
   `gfx::OffscreenTexture` / the render-target contract. **This does not exist for GL today** and is
   the single largest risk. Vulkan's offscreen + depth path is more complete — verify it supports a
   depth attachment, but it likely needs less new code than GL.
   - Verify color writes are not clobbered when a depth attachment is bound (see §2.3 sharp edge).

### Phase 4 — shader `.cpp` + attribute/texture registration

8. Add the `.cpp` files mirroring `src/mbgl/shaders/mtl/{shadow_depth,fill_extrusion_shadow,ground_shadow}.cpp`
   for each backend (attribute locations, UBO bindings, texture sampler bindings using
   `idFillExtrusionShadowTexture0..3` / `idGroundShadowTexture0..3`).

### Phase 5 — backend gates + light integration (mostly inspection)

9. Confirm `render_orchestrator.cpp` / `render_fill_extrusion_layer.cpp` compile for GL/Vulkan
   (they're backend-agnostic but were only exercised on Metal). Confirm the `MLN_RENDER_3D_ENHANCEMENTS`
   gate compiles & no-ops correctly on both Android backends.

### Phase 6 — validation

10. Shader compilation (both backends) → packed-depth round-trip test (pack then unpack ≈ identity)
    → on-device A/B with the gate off/on (byte-identical when off) → real HataHub Android build
    (release/embedded-JS build; see memory `project_mobile_android_killed_call_build.md` for the
    Android build gotchas). Use the same headless `mbgl-render` approach for GL on Linux/macOS if
    available.

**Net new Android files:** 6 shader headers + 6 shader `.cpp` + GL `offscreen_texture.{hpp,cpp}`.
**Modified:** `shader_manifest.hpp`, `gl/renderer_backend.cpp`, `vulkan/renderer_backend.cpp`.

---

## 4. Web port plan (MapLibre GL JS)

**This is a separate codebase** (`maplibre-gl-js`, TypeScript + WebGL2) — none of the C++ above is
reused. It's a from-scratch re-implementation guided by the same architecture.

**Verdict (from research):** ✅ **Feasible, no technical blockers.** WebGL2 has depth textures and
MRT; the architecture transfers. It's a **large fork**, not a small patch. **~10–13 weeks**
(6–10 weeks core + 2–3 weeks testing).

**Why fork, not upstream:** no upstream maintainer appetite for shadow rendering, and a
WebGL2-only feature is controversial for a library that still supports WebGL1 paths. Keep it a
HataHub fork, same as the native fork.

**Suggested fork layout:**

- New: `src/shaders/_shadow_depth.{vertex,fragment}.glsl` (caster)
- New: `src/render/shadow_pass.ts`, `src/render/shadow_manager.ts` (orchestration + cascades)
- Modified: `src/render/draw_fill_extrusion.ts` (receiver sampling), `src/render/painter.ts`/`renderer`
  (insert the caster pass), `src/style/light.ts` (read `cast-shadows`/`shadow-intensity`),
  `src/style/style_layer/fill_extrusion*.ts` (wall-normal / wallness attribute)

**Phasing** (start with spikes to de-risk):

1. Spike (1w): render a depth texture from the sun; visualize it.
2. Spike (1w): sample shadow factor in a receiver overlay.
3. Caster pass plumbing (2w) → 4. Receiver shader + wall suppression (1w) → 5. Cascaded maps (2w) →
   6. `light` style API + ground receiver (1w) → 7. polish & cross-browser testing (2–3w).

**Lower-quality alternative:** screen-space ambient occlusion / contact shadows (~3 weeks) if
directional cast shadows prove too costly — but quality is materially worse and it won't match the
native look.

**Today:** the web app serves a **separate bundled style** (`apps/web/public/map/style/light.json`,
plain MapLibre GL JS, no fork) → **web shows no shadows** until this port lands. That's expected.

---

## 5. Deployment / production notes (iOS, already done — replicate for others)

- **Production tile/style host:** R2 bucket `hatahub-tiles`, key `style/light.json`, served at
  `https://map.hatahub.com.ua/style/light.json`. Deploy via
  `aws s3 cp … --endpoint-url <R2_ENDPOINT>` using `TILES_R2_ACCESS_KEY`/`TILES_R2_SECRET_KEY`
  from `.env.local`. **This is production — handle credentials only through that mechanism.**
- **Always base a deploy on the LIVE R2 style**, not the repo copy. The repo copy has drifted
  behind before (live = 39 layers incl. building-hover/listings, street-highlight, split
  poi-label; a stale repo copy was 35 layers and dropped the 4 live-only layers). The deployed
  style has **39 layers, `shadow-intensity: 0.12`** — verify layer count + intensity after every push.
- **Cache-bust the app:** bump `MAP_STYLE_VERSION` in `apps/mobile/lib/maplibre/style.ts`
  (currently **9**) on every style push — the CDN caches per full URL up to a day.
- **Repo style of record:** `infrastructure/tiles/style/light.json` (committed `fd04381fd`),
  reconciled to the 39-layer live style + shadow light. Biome pre-commit formats JSON — run
  `bunx biome format --write` on it and don't leave scratch `light.*.json` files around (they fail
  `biome check .`).
- **iOS prod build & the local fork:** the app links MapLibre via SPM at the **local path**
  `/Users/berg/Work/maplibre-native/dist-spm`. The fork branch is **not pushed**, so any CI/EAS
  build host needs that framework present (built with `--//:renderer=metal`, xcframework target
  `//platform/ios:MapLibre.dynamic`, unzipped into `dist-spm/MapLibre.xcframework`). **dist-spm
  binaries are build artifacts — commit source only, never the binaries.** Resolving the
  fork-is-local-only constraint for EAS/CI is an open item before a clean iOS prod pipeline.

---

## 6. Open items & risk register

### Blockers / decisions needed
- **Android:** implement GL offscreen render target with a depth attachment (§3 Phase 3).
- **Web:** confirm fork (vs. SSAO alternative) and allocate the ~10–13 week budget.
- **iOS CI/EAS:** decide how the build host gets the local-only fork framework (§5).

### Risk matrix
| Risk | Where | Mitigation |
|---|---|---|
| GL has no offscreen depth target | Android | §3 Phase 3 — biggest unknown; spike it first |
| GL clip-space Z `[-1,1]` vs `[0,1]` | Android GL, Web | Single remap in caster; match unpack |
| Color write clobbered by depth attachment | All | §2.3 — get color+depth attachment combo right |
| MSL→GLSL/SPIR-V shader divergence | Android | Reference `gl/`+`vulkan/` `fill_extrusion.hpp` exactly |
| Cross-cascade banding / seam | All | Hard transition shipped on iOS; revisit only if visible |
| Packed-depth precision loss | All | Keep the exact RGBA8 pack/unpack pair from MSL |
| Mobile GPU fill cost (extra passes) | Android | Gate on `MLN_RENDER_3D_ENHANCEMENTS`; 2048 map; A/B perf |
| Fork drift from upstream | All | Fork-only, keep gate; rebase deliberately |
| Web WebGL1 fallback | Web | WebGL2-only feature; degrade gracefully (no shadows) |
| R2 style drift / wrong layer count | Deploy | Diff against live before every push; assert 39 layers |

### Pre-ship verification checklist (per platform)
- [ ] Gate off → renderer byte-identical to stock (regression guard)
- [ ] `shadow_frustum` + `shadow_sun` tests pass (and a packed-depth round-trip test on the new backend)
- [ ] Headless `mbgl-render` A/B at z18 pitched, Kyiv center → ground + wall + roof look correct, no wall smear
- [ ] On-device A/B (sim + real device), shadows world-anchored under rotate/pitch
- [ ] Real HataHub build links the fork, style version bumped, prod R2 layer count + intensity verified
