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
  so `glDrawElementsInstanced` and `glVertexAttribDivisor` are **core in GLES 3.0**.
  **CAVEAT (must verify on device):** Android requests an **ES2** context —
  `EGL_CONTEXT_CLIENT_VERSION = 2` in `platform/.../egl/EGLContextFactory.java:19` and likewise in
  `.../textureview/GLTextureViewRenderThread.java:252` — even though the config asks for
  `EGL_OPENGL_ES3_BIT` (`.../egl/EGLConfigChooser.java:283`). The map renders today only because
  drivers hand back a ≥ES3-capable context. Relying on that for ES3-core instancing is unsafe, so
  Layer 0 (below) **explicitly requests an ES3 context and logs `GL_VERSION` on device** before any
  instancing path is enabled. If a target device only yields a true ES2 context, the fallback is the
  `GL_EXT_instanced_arrays` extension (`glDrawElementsInstancedEXT` / `glVertexAttribDivisorEXT`),
  assessed at that point — not assumed.
- `include/mbgl/shaders/gl/shadow_depth_instanced.hpp` is an explicit placeholder: its own header
  comment says it exists only so the all-backend shader manifest compiles, and "If a GL instancing
  path is ever added, replace this with a real port of mtl/shadow_depth.hpp's ShadowDepthInstancedShader."
- The shared `#if MLN_USE_FILL_EXTRUSION_INSTANCING` branches in
  `src/mbgl/renderer/buckets/fill_extrusion_bucket.{hpp,cpp}` and
  `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` already implement the instanced data
  model (per-edge instance vertices + a shared static unit-quad). They are simply not compiled for GL.

### CRITICAL: GLES 3.0 cannot replicate Metal's instanced data model (decided 2026-06-17)

The Metal/Vulkan instanced shaders read a **raw outline buffer indexed by `instance_id` and its
neighbors** — `outline[instanceID + vertx.pos.x].pos` for the edge's far endpoint, and
`outline[instanceID-1 / +1 / +2]` for crease-aware smooth normals (see
`include/mbgl/shaders/mtl/fill_extrusion.hpp` `FillExtrusionInstancedShader`). GLES 3.0 instanced
attributes (`glVertexAttribDivisor`) expose **only the current instance's** attributes — there is no
buffer-indexing-by-`gl_InstanceID`, no SSBO (ES 3.1), and no texture-buffer objects (ES 3.2). So the
GL shader **cannot** "extrude walls exactly as Metal does."

**Decision: Option (c) — edge-indexed instances with precomputed normals.** The GL path emits **one
instance per footprint EDGE** (not per vertex), each instance carrying **both endpoint positions +
the precomputed smoothed wall normal + edgeDistance/base/height/color** as plain divisor-1
attributes. The GL vertex shader reads **only current-instance attributes** (no neighbor/buffer
access). The smoothed normal reuses the bucket's existing `blendNormal` crease-aware logic that
already lives in the non-instanced `#else` path — so smooth-facade parity comes from existing code,
not a shader port.

Consequence for the unification goal: GL shares the **render-layer flow and shader concepts** with
Metal/Vulkan, but builds a **GL-specific edge-indexed instance buffer** distinct from Metal's
vertex-indexed `sharedVertices` reuse. Unification is at the concept/flow level, not one identical
buffer path. (Rejected alternative: Option (b), upload the outline as an integer texture and
`texelFetch` by `gl_InstanceID` — mirrors Metal exactly but vertex texture fetch is slow on mobile
Mali/Adreno GPUs, undermining the perf goal.)

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

## Architecture — five layers (bottom-up)

### Layer 0 — ES3 context guarantee (new, small, must land first)
Before any instanced GL draw can be relied on, the backend must run on a real ES3 context.
- Bump `EGL_CONTEXT_CLIENT_VERSION` 2 → 3 in `platform/.../egl/EGLContextFactory.java:19` and the
  TextureView path `.../textureview/GLTextureViewRenderThread.java:252` (config already requests
  `EGL_OPENGL_ES3_BIT`).
- Log `glGetString(GL_VERSION)` on the render thread at context creation; the GL instancing gate
  (Layer 4) refuses to enable if the runtime context reports < ES 3.0.
- If a real target device only yields ES2, branch to the `GL_EXT_instanced_arrays` extension entry
  points — assessed then, not assumed.

### Layer 1 — GL instanced-draw primitives (backend-local, new)
The only genuinely new GL capability. Changes nothing about Metal/Vulkan. This layer is **larger than
a divisor flag** — verified against the current code, *all* of the following must land together or the
shared render-layer path compiles while per-instance data is silently unbound:
- **Divisor carried end-to-end, not just on `VertexAttribute`.** Today the VAO setter receives only
  `gfx::AttributeBinding` (`src/mbgl/gl/value.cpp:610`), which has **no divisor field**
  (`src/mbgl/gfx/attribute.hpp:116`). Add divisor to `gfx::AttributeBinding` and thread it through
  `buildAttributeBindings`, the binding equality/state caching, and `value::VertexAttribute::Set` so
  it survives to the `glVertexAttribPointer` + `glVertexAttribDivisor` call.
- **GL shader instance-attribute metadata.** `ShaderProgramGL::create()` currently funnels every
  active GLSL attribute into one `vertexAttributes` array and leaves `instanceAttributes` empty
  (`src/mbgl/shaders/gl/shader_program_gl.cpp:161` ff). Populate per-attribute instance classification
  (from the shader's attribute info / instance set) so the drawable knows which attributes are
  per-instance.
- **Separate instance buffer upload + merged VAO bindings.** `DrawableGL` upload today builds
  bindings only from `shader->getVertexAttributes()` + `vertexAttributes`
  (`src/mbgl/gl/drawable_gl.cpp:178`). Add instance vertex-attribute upload (own buffer(s)) and merge
  per-vertex + per-instance bindings into the VAO, with the instance bindings marked divisor 1.
- **Instanced draw.** Add `gl::Context::drawInstanced(drawMode, indexOffset, indexLength,
  instanceCount)` calling `glDrawElementsInstanced`, alongside the existing `draw`. `DrawableGL`
  carries an `instanceCount` (0 → existing `draw`; >0 → `drawInstanced`).
- **VAO cache invalidation:** divisor changes must invalidate the cached binding state.
- **Interface:** other backends untouched; unit-testable with a trivial instanced quad
  (divisor + `glDrawElementsInstanced` drawing N quads correctly).

### Layer 2 — GL instanced shaders (new GLSL ES 3.0 ports)
Real ports of the three shaders the manifest already enumerates for GL as placeholders:
- `FillExtrusionInstancedShader`
- `FillExtrusionPatternInstancedShader`
- `ShadowDepthInstancedShader` (replaces the documented stub in `gl/shadow_depth_instanced.hpp`)

Per the Option (c) decision above, they consume the static unit-quad as per-vertex geometry plus
**edge-indexed** per-instance attributes (divisor 1): `pos0`, `pos1` (the edge's two endpoints),
`normal` (precomputed smoothed wall normal), `edgeDistance`, and data-driven base/height/color. The
GL vertex shader extrudes the wall from the unit quad reading **only the current instance** — `pos.x`
selects `pos0`/`pos1`, `pos.y` selects base/height — with no neighbor or buffer indexing. Register in
`src/mbgl/gl/shader_info.cpp` + `src/mbgl/gl/renderer_backend.cpp` (today deliberately not registered
for these; note `include/mbgl/shaders/gl/fill_extrusion_instanced.hpp` already exists as an empty
placeholder to fill in).
- **Smooth curved-facade normals** are precomputed in the bucket via the existing `blendNormal` logic
  (already in the `#else` path) and passed as the per-instance `normal` attribute — no in-shader
  neighbor reads, no faceted regression.

### Layer 3 — Shared bucket / render-layer path (compile for GL)
The existing `#if MLN_USE_FILL_EXTRUSION_INSTANCING` branches get compiled for GL, gated by Layer 4.

### Layer 4 — The rollout gate (`MLN_GL_FE_INSTANCING`, compile-time, default off → on at collapse)
Selects instanced vs. non-instanced for the GL backend. During bring-up: off by default; A/B by
building with it on vs. the current shipping AAR. At collapse: default on, then delete the gate and
the GLES `#else` wall-quad path.

## Data flow (GL instanced path)

1. **Tile load → bucket.** On the GL instanced path, the bucket emits an **edge-indexed instance
   record** per footprint edge: `{pos0, pos1, normal, edgeDistance, discard}` (normal from the
   existing `blendNormal`), plus the data-driven base/height/color instance streams. Roof triangles
   are emitted as today.
2. **Frame → render layer.** Binds the shared static unit-quad (per-vertex, divisor 0) and the
   edge-instance attributes (divisor 1), builds one instanced color drawable (+ depth drawable) per
   tile; roof triangles remain a normal (non-instanced) drawable.
3. **Draw → GL backend.** `DrawableGL` with `instanceCount = edgeCount` → `Context::drawInstanced`
   → `glDrawElementsInstanced(GL_TRIANGLES, 6, …, instanceCount)`. The vertex shader extrudes each
   wall from the unit quad using **the current instance's** `pos0`/`pos1`/normal/base/height — no
   neighbor or buffer indexing.
4. **Shadows** through `ShadowDepthInstancedShader`. The static/outline geometry and the per-edge
   instance positions may be shared, **but the wall caster's base/height are NOT a plain reuse of the
   visible drawable's buffer.** The current code deliberately creates separate wall-caster instance
   attributes and de-interleaves base/height into owned `Float2` buffers
   (`src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp:540`), then binds those independently to
   the instanced caster (`:967`). The GL path must preserve this independent-layout handling, not
   collapse it into a single shared interleaved buffer.

## §4 — Cadence sub-track (60fps lever; orthogonal, bundled)

Touches only Android render scheduling — none of Layers 0–4. The active GL render-thread loops (the
files that actually drive frames, verified) are:
- `platform/.../renderer/surfaceview/MapLibreGLSurfaceView.java:499` — GL SurfaceView render loop
  (this is the surface the 2026-06-13 benchmark measured).
- `platform/.../renderer/textureview/GLTextureViewRenderThread.java:87` — TextureView's separate
  request loop (if any HataHub screen uses TextureView).
- `platform/.../cpp/android_renderer_frontend.cpp:121` — native invalidation / `requestRender` path.
- plus the shared `MapRenderer*.java` / `MapRendererScheduler` and `src/cpp/map_renderer.cpp` wake path.

Investigation order:
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

- **Layer 0:** on-device log confirming `GL_VERSION` ≥ ES 3.0 after the context-version bump (Honor
  + at least one second device).
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
