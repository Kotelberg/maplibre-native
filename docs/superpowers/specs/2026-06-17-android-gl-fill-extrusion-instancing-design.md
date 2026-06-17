# Android GL Fill-Extrusion Instancing + 60fps Cadence — Design

**Date:** 2026-06-17
**Fork:** ~/Work/maplibre-native, branch `gl/fill-extrusion-instancing` (off `shadows/s1-shadowmap-ios` @ 0a81f793)
**Status:** Design approved; ready for implementation plan.

## Goal

Bring the iOS-quality 3D building experience to Android by porting the **fill-extrusion
instancing path** (today Metal/Vulkan only) to the **OpenGL ES backend**, and — bundled in the
same milestone — fixing the Android **render-loop cadence** so the map sustains ~60fps.

Vulkan is explicitly **out of scope** for this milestone (GL first).

### Why (all four confirmed with the user)

1. **Tile-load smoothness / memory** — the current GLES `#else` path emits explicit wall quads on
   the CPU (≈4× the geometry of the instanced edge representation). Instancing cuts tile-gen time,
   upload, and resident memory → fewer load hitches.
2. **Visual parity with iOS** — bring shadows + smooth curved-facade shading onto the same path iOS
   uses, not a divergent Android-only one.
3. **60fps steady-state** — the user wants demonstrably-60fps Android. NOTE: the 2026-06-13 Honor
   benchmark found 3D content added **zero** steady-state pan cost and the ~30fps cap was the
   `requestRender`→present round-trip spanning 2 vsyncs (render-loop scheduling / vendor power
   policy), **not** geometry. So 60fps is a *separate lever* from instancing — addressed by the
   cadence sub-track (§4), bundled into this milestone at the user's request.
4. **Architectural correctness** — unify Android onto the same instanced code path as Metal/Vulkan
   so the GLES `#else` fork (smooth-shading, etc.) stops diverging and needing separate maintenance.

## Background — current state (verified 2026-06-17)

- `MLN_USE_FILL_EXTRUSION_INSTANCING` is defined as `(MLN_RENDER_BACKEND_METAL ||
  MLN_RENDER_BACKEND_VULKAN)` in `include/mbgl/shaders/layer_ubo.hpp:88`. GL is excluded.
- The GL backend has **zero** instanced-rendering plumbing: `gl::Context::draw`
  (`src/mbgl/gl/context.cpp:767`) calls plain `glDrawElements`; there is no instance-divisor concept
  in the GL vertex-attribute code, and `glVertexAttribDivisor` is never called anywhere.
- The GL backend already compiles `#version 300 es` (`src/mbgl/shaders/gl/shader_program_gl.cpp:126`),
  so `glDrawElementsInstanced` and `glVertexAttribDivisor` are **core in GLES 3.0** — no extension
  needed.
- `include/mbgl/shaders/gl/shadow_depth_instanced.hpp` is an explicit placeholder: its own header
  comment says it exists only so the all-backend shader manifest compiles, and "If a GL instancing
  path is ever added, replace this with a real port of mtl/shadow_depth.hpp's ShadowDepthInstancedShader."
- The shared `#if MLN_USE_FILL_EXTRUSION_INSTANCING` branches in
  `src/mbgl/renderer/buckets/fill_extrusion_bucket.{hpp,cpp}` and
  `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` already implement the instanced data
  model (per-edge instance vertices + a shared static unit-quad). They are simply not compiled for GL.

### Instanced data model (already in the codebase, Metal/Vulkan today)

- Instanced bucket layout vertex: `Vertex<attributes::pos, attributes::ed_discard>` — one *instance*
  per footprint edge (`pos` + edgeDistance + discard flag).
- Non-instanced (`#else`, GLES today) layout vertex: `Vertex<attributes::pos, attributes::normal_ed>`
  — explicit per-vertex wall quads with packed normals (≈4× the geometry).
- The render layer binds `RenderStaticData::fillExtrusion{Vertices,TriangleIndices,Segments}` (a unit
  quad) as per-vertex geometry, and the bucket's edge vertices + per-instance base/height/color as
  per-instance attributes. The vertex shader extrudes each wall from the unit quad.

## Approach (chosen: B / B1)

Build the GL instanced path as a **new, gated** path so the proven non-instanced `#else` path stays
intact, verify parity + perf, then **collapse** (flip the default on, delete the old path) to reach
full unification. This is incremental and on-device-verifiable at every step — important because
GLES depth precision has repeatedly surfaced bugs Metal hid (z-fighting, dark walls, stencil-clip).

**Fallback model: B1 (compile-time gate, two builds).** The GL instanced path lives behind
`#if MLN_GL_FE_INSTANCING`. A/B comparison is the **new instanced build vs. the current shipping
(non-instanced) build** — the fallback *is* today's production AAR, so there is no dual-representation
memory cost and no throwaway CPU-expansion code. (B2, a runtime single-binary toggle, was considered
and rejected: it would force the bucket to store both representations or synthesize quads at draw
time, adding code deleted at collapse anyway. Revisit only if a deployed in-app kill-switch is wanted.)

Rejected: **Approach A** (big-bang macro flip, no fallback — too risky on GLES); **Approach C**
(instance only visible walls, leave shadows/smooth-normals behind — fails the parity + unification goals).

## Architecture — four independent layers (bottom-up)

### Layer 1 — GL instanced-draw primitives (backend-local, new)
The only genuinely new GL capability. Changes nothing about Metal/Vulkan.
- Add a **per-attribute instance divisor** to `gfx::VertexAttribute` / `VertexAttributeArrayGL`
  (default 0 = per-vertex; 1 = per-instance).
- Add `gl::Context::drawInstanced(drawMode, indexOffset, indexLength, instanceCount)` calling
  `glDrawElementsInstanced`, alongside the existing `draw`.
- In the VAO attribute-bind path (`src/mbgl/gl/value.cpp`), call `glVertexAttribDivisor` for any
  attribute whose divisor is 1.
- `DrawableGL` carries an `instanceCount` (0 → existing `draw`; >0 → `drawInstanced`).
- **Interface:** other backends untouched; unit-testable with a trivial instanced quad.

### Layer 2 — GL instanced shaders (new GLSL ES 3.0 ports)
Real ports of the three shaders the manifest already enumerates for GL as placeholders:
- `FillExtrusionInstancedShader`
- `FillExtrusionPatternInstancedShader`
- `ShadowDepthInstancedShader` (replaces the documented stub in `gl/shadow_depth_instanced.hpp`)

They consume the static unit-quad as per-vertex geometry plus per-instance `pos`/`ed_discard`/base/
height/color attributes (divisor 1), extruding walls in the vertex shader exactly as the Metal
versions do. Register in `src/mbgl/gl/shader_info.cpp` + `src/mbgl/gl/renderer_backend.cpp` (today
deliberately not registered for these).
- **Smooth curved-facade normals** (currently only in the GLES `#else` bucket) must be re-derived in
  the instanced wall shader from the edge direction, or buildings regress to faceted shading.

### Layer 3 — Shared bucket / render-layer path (compile for GL)
The existing `#if MLN_USE_FILL_EXTRUSION_INSTANCING` branches get compiled for GL, gated by Layer 4.

### Layer 4 — The rollout gate (`MLN_GL_FE_INSTANCING`, compile-time, default off → on at collapse)
Selects instanced vs. non-instanced for the GL backend. During bring-up: off by default; A/B by
building with it on vs. the current shipping AAR. At collapse: default on, then delete the gate and
the GLES `#else` wall-quad path.

## Data flow (GL instanced path)

1. **Tile load → bucket.** `FillExtrusionBucket::addFeature` emits the instanced layout: one
   `{pos, ed_discard}` vertex per footprint edge (the instances) — less geometry than today's `#else`.
2. **Frame → render layer.** Binds the shared static unit-quad (per-vertex), the bucket's edge
   vertices + per-instance base/height/color (divisor 1), builds one instanced drawable per tile;
   roof triangles remain a normal (non-instanced) drawable.
3. **Draw → GL backend.** `DrawableGL` with `instanceCount = edgeCount` → `Context::drawInstanced`
   → `glDrawElementsInstanced(GL_TRIANGLES, 6, …, instanceCount)`. The vertex shader extrudes each
   wall from the unit quad using the instance's two endpoints + base/height.
4. **Shadows** reuse the same instance buffer through `ShadowDepthInstancedShader`.

## §4 — Cadence sub-track (60fps lever; orthogonal, bundled)

Touches only Android render scheduling (`platform/android/.../renderer/MapRenderer*.java`,
`MapLibreSurfaceView.java`, `src/cpp/map_renderer.cpp`) — none of Layers 1–4. Investigation order:
- **Render mode:** confirm whether MapLibre-RN drives the SurfaceView renderer with a request/wait
  handoff that adds a vsync of latency vs. continuous; inspect `MapRendererRunnable` /
  `MapRendererScheduler` and the C++ wake path.
- **Choreographer alignment:** ensure rendering is driven off a vsync-aligned `Choreographer` frame
  callback rather than a posted runnable landing mid-frame.
- **Vendor power/thermal policy:** rule in/out via forced performance governor / a second device /
  `dumpsys SurfaceFlinger` present cadence. If purely vendor frame-pacing, document as a
  device-specific ceiling rather than chasing a phantom code fix.
- Each finding lands as its own commit so the fps delta is attributable.

## Verification

- **Layer 1:** minimal headless-GL instanced-quad render test (divisor + `glDrawElementsInstanced`).
- **Layer 2/3 parity:** render HataHub style z16/pitch — new instanced build vs. current
  non-instanced build; pixel-diff (AE threshold like existing model render tests).
- **On-device (Honor, mandatory):** building walls, shadows, smooth curved facades (the Парус case),
  selection highlight, **close-zoom z-fighting** (GLES depth precision is the known risk — verify
  on-device, not just headless). SurfaceFlinger `--latency` for the perf delta, measured as in the
  2026-06-13 benchmark (clear ring → one continuous swipe → read immediately).
- **Cadence:** sustained ~60fps present cadence on the Honor during continuous pan.

## Risks

- **GLES depth precision** (< Metal) — repeatedly surfaced z-fighting / dark-wall bugs Metal hid.
  Mitigation: on-device close-zoom verification is mandatory.
- **Metal-specific assumptions** in the shared instanced branches (attribute interleaving; the
  caster's independent per-instance layouts noted in `render_fill_extrusion_layer.cpp`) each need a
  GL-correct equivalent.
- **Smooth normals regression** — see Layer 2; carry the curved-facade win into the instanced shader.
- **60fps may be vendor-capped** — if the cadence sub-track finds a hardware/vendor frame-pacing
  ceiling, that is documented, not forced.

## Packaging discipline (per project memory)

Every fork commit affecting natives → rebuild AAR (`./gradlew :MapLibreAndroidTestApp:assembleOpenglDebug`,
arm64), `rm -rf ~/.gradle/caches/modules-2/files-2.1/org.maplibre.gl`, republish to `~/.m2`, rebuild
the HataHub app. Fork-only — **no upstream PRs**.

## Out of scope

- Vulkan backend (deferred by user).
- iOS / Metal changes (already shipping the instanced path).
- Upstream contributions.
