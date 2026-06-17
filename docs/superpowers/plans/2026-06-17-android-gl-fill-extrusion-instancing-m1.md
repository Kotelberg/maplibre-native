# Android GL Fill-Extrusion Instancing (Plan 1 / GL path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the Metal/Vulkan instanced fill-extrusion path to the OpenGL ES backend so Android renders building walls (+ shadows + smooth facades) via `glDrawElementsInstanced`, behind a compile-time gate, verified headlessly and on-device.

**Architecture:** Five layers, bottom-up — (0) guarantee a real ES3 context; (1) add instance-divisor + instanced-draw primitives to the GL backend; (2) author GL ES 3.0 instanced shaders using an **edge-indexed** data model (one instance per footprint edge carrying both endpoints + a precomputed smoothed normal — NOT Metal's vertex-indexed buffer, since GLES 3.0 can't index a buffer by `gl_InstanceID`); (3) emit edge-instance data in the bucket and build instanced drawables in the render layer; (4) gate it all behind `MLN_GL_FE_INSTANCING`. Verified via the headless GL build `build-gl-check` (unit tests + render parity) and on-device on the Honor.

**Tech Stack:** C++17, OpenGL ES 3.0 / GLSL `#version 300 es`, MapLibre Native mbgl renderer, CMake/Ninja (`build-gl-check`), GoogleTest (`mbgl-test-runner`), `mbgl-render` headless, ImageMagick `compare`, Android Gradle (`assembleOpenglDebug`), adb.

## Global Constraints

- **Backend gate:** GL instancing lives behind `#if MLN_GL_FE_INSTANCING` (compile-time, default **0** during bring-up). It must NOT alter Metal/Vulkan behavior. `MLN_USE_FILL_EXTRUSION_INSTANCING` stays `(METAL || VULKAN)` for now; the GL instanced path is selected by `MLN_GL_FE_INSTANCING`, independent of that macro.
- **Data model:** Option (c) — edge-indexed instances; the GL vertex shader reads ONLY current-instance attributes. No SSBO (ES 3.1), no texture-buffer objects (ES 3.2), no vertex `texelFetch` of outline data. GLSL is `#version 300 es`.
- **No faceted regression:** smoothed wall normals are precomputed in the bucket via the existing `blendNormal` logic and passed as a per-instance `normal` attribute.
- **Fallback model B1:** the proven non-instanced `#else` GL path stays intact; A/B is the new gated build vs. the current shipping AAR. The `#else` deletion (collapse) is explicitly OUT of this plan (follow-up after parity+perf sign-off).
- **No upstream PRs.** Fork-only (Kotelberg/maplibre-native).
- **Packaging discipline (per project memory):** after native changes, rebuild AAR `./gradlew :MapLibreAndroidTestApp:assembleOpenglDebug` (arm64), `rm -rf ~/.gradle/caches/modules-2/files-2.1/org.maplibre.gl`, republish to `~/.m2`, rebuild HataHub.
- **Headless GL build dir:** `build-gl-check` (already configured: `MLN_WITH_OPENGL=ON`, `MLN_WITH_METAL=OFF`, EGL). Build with `cmake --build build-gl-check --target <t> -j`.
- **GLES depth precision** is lower than Metal — every visual task MUST be confirmed at close zoom on-device, not only headless.

## File Structure

**New files**
- `include/mbgl/shaders/gl/fill_extrusion_instanced.hpp` — EXISTS as empty placeholder; fill in the GL `FillExtrusionInstancedShader` source (Task 6).
- `include/mbgl/shaders/gl/fill_extrusion_pattern_instanced.hpp` — may need creating if not present (Task 7); GL `FillExtrusionPatternInstancedShader`.
- `test/gl/instancing.test.cpp` — new GL unit tests for divisor plumbing + bucket edge-instance emission (Tasks 3, 5, 9).

**Modified files**
- `include/mbgl/shaders/layer_ubo.hpp` — define `MLN_GL_FE_INSTANCING` (Task 1).
- `src/mbgl/gfx/attribute.hpp` — `AttributeBinding::instanceDivisor` (Task 2).
- `src/mbgl/gl/value.cpp` — `glVertexAttribDivisor` in `VertexAttribute::Set` (Task 2).
- `src/mbgl/gl/context.{hpp,cpp}` — `Context::drawInstanced` (Task 4).
- `src/mbgl/gl/vertex_array.cpp` — pass divisor through VAO bind (Task 2).
- `include/mbgl/gl/drawable_gl.hpp`, `src/mbgl/gl/drawable_gl.cpp`, `src/mbgl/gl/drawable_gl_impl.hpp` — instance attributes + `instanceCount` + instanced draw (Task 5).
- `include/mbgl/shaders/gl/shader_info.hpp`, `src/mbgl/shaders/gl/shader_info.cpp` — `instanceAttributes` lists for the instanced shaders (Task 8).
- `src/mbgl/shaders/gl/shader_program_gl.cpp` (= `src/mbgl/shaders/gl/shader_program_gl.cpp`) — populate `instanceAttributes` in `create()` (Task 8).
- `src/mbgl/gl/renderer_backend.cpp` — register the 3 instanced shaders for GL (Task 8).
- `src/mbgl/renderer/buckets/fill_extrusion_bucket.{hpp,cpp}` — edge-instance layout + emission under `MLN_GL_FE_INSTANCING` (Task 3).
- `src/mbgl/renderer/layers/render_fill_extrusion_layer.{hpp,cpp}` — GL instanced builders + gate (Task 10).
- `include/mbgl/shaders/gl/shadow_depth_instanced.hpp` — real GL caster (Task 11).
- `platform/android/.../egl/EGLContextFactory.java`, `.../textureview/GLTextureViewRenderThread.java`, native GL context init — ES3 context + `GL_VERSION` log (Task 0).

---

### Task 0: Guarantee an ES3 context + log GL_VERSION (Layer 0)

**Files:**
- Modify: `platform/android/MapLibreAndroid/src/opengl/java/org/maplibre/android/maps/renderer/egl/EGLContextFactory.java:19`
- Modify: `platform/android/MapLibreAndroid/src/opengl/java/org/maplibre/android/maps/renderer/textureview/GLTextureViewRenderThread.java:252`
- Modify: `src/mbgl/gl/context.cpp` (in `Context::initializeExtensions`/first-use, add a one-time `GL_VERSION` log)

**Interfaces:**
- Produces: an ES3 GL context on Android; a logged `GL_VERSION` string at startup. No code symbol consumed by later tasks — this is a runtime precondition for Tasks 2/4/5 to function on-device.

- [ ] **Step 1: Bump the SurfaceView context to ES3**

In `EGLContextFactory.java`, change the attribute list from client version 2 to 3:
```java
// 0x3098 == EGL_CONTEXT_CLIENT_VERSION
int[] attrib_list = {0x3098, 3, EGL10.EGL_NONE};
```

- [ ] **Step 2: Bump the TextureView context to ES3**

In `GLTextureViewRenderThread.java:252`, change the same `EGL_CONTEXT_CLIENT_VERSION` value from `2` to `3`. (Search the file for `0x3098` / `EGL_CONTEXT_CLIENT_VERSION`.)

- [ ] **Step 3: Log GL_VERSION once on the render thread**

In `src/mbgl/gl/context.cpp`, in the context initialization path (where extensions are first queried), add a one-time log:
```cpp
Log::Info(Event::OpenGL, std::string("GL_VERSION: ") +
          reinterpret_cast<const char*>(MBGL_CHECK_ERROR(glGetString(GL_VERSION))));
```

- [ ] **Step 4: Build the GL desktop build to confirm no regression**

Run: `cmake --build build-gl-check --target mbgl-render -j`
Expected: builds clean (desktop GL context is already ≥3.0; this step just guards the C++ log edit compiles).

- [ ] **Step 5: On-device GL_VERSION check**

Build + install the Android test app, then:
Run: `adb logcat -d | grep "GL_VERSION"`
Expected: a line reporting `OpenGL ES 3.x` on the Honor (and at least one second device).
If any target reports `< 3.0`, STOP — the `GL_EXT_instanced_arrays` fallback (out of this plan) is required; record it and consult.

- [ ] **Step 6: Commit**
```bash
git add platform/android/MapLibreAndroid/src/opengl/java/org/maplibre/android/maps/renderer/egl/EGLContextFactory.java \
        platform/android/MapLibreAndroid/src/opengl/java/org/maplibre/android/maps/renderer/textureview/GLTextureViewRenderThread.java \
        src/mbgl/gl/context.cpp
git commit -m "feat(gl): request ES3 context on Android + log GL_VERSION (Layer 0)"
```

---

### Task 1: Define the `MLN_GL_FE_INSTANCING` gate

**Files:**
- Modify: `include/mbgl/shaders/layer_ubo.hpp:88` (next to the `MLN_USE_FILL_EXTRUSION_INSTANCING` define)

**Interfaces:**
- Produces: macro `MLN_GL_FE_INSTANCING` (0/1). Consumed by Tasks 3, 8, 10, 11. Default 0.

- [ ] **Step 1: Add the gate macro**

After the existing `#define MLN_USE_FILL_EXTRUSION_INSTANCING (...)` line, add:
```cpp
// GL-only edge-indexed fill-extrusion instancing (Plan 1). Independent of
// MLN_USE_FILL_EXTRUSION_INSTANCING (which stays Metal/Vulkan). Default off during bring-up;
// flip to 1 to enable the OpenGL instanced path. See docs/superpowers/specs/2026-06-17-...
#if !defined(MLN_GL_FE_INSTANCING)
#define MLN_GL_FE_INSTANCING 0
#endif
```

- [ ] **Step 2: Build to confirm the define is harmless at 0**

Run: `cmake --build build-gl-check --target mbgl-render -j`
Expected: clean build (no behavioral change).

- [ ] **Step 3: Commit**
```bash
git add include/mbgl/shaders/layer_ubo.hpp
git commit -m "feat(gl): add MLN_GL_FE_INSTANCING compile gate (default off)"
```

---

### Task 2: Carry an instance divisor through `AttributeBinding` → GL VAO bind

**Files:**
- Modify: `src/mbgl/gfx/attribute.hpp:116` (the `AttributeBinding` class + its `operator==`)
- Modify: `src/mbgl/gl/value.cpp:610` (`VertexAttribute::Set`)
- Modify: `src/mbgl/gl/vertex_array.cpp` (the bind loop that calls `VertexAttribute::Set`)
- Test: `test/gl/instancing.test.cpp` (new)

**Interfaces:**
- Produces: `AttributeBinding::instanceDivisor` (`uint32_t`, default 0). `VertexAttribute::Set` issues `glVertexAttribDivisor(location, divisor)`. Consumed by Tasks 5/10 (instance bindings set divisor 1).

- [ ] **Step 1: Write the failing test**

Create `test/gl/instancing.test.cpp`:
```cpp
#if MLN_RENDER_BACKEND_OPENGL
#include <mbgl/test/util.hpp>
#include <mbgl/gfx/attribute.hpp>

using namespace mbgl;

TEST(GLInstancing, AttributeBindingCarriesDivisor) {
    gfx::AttributeBinding a{};
    a.instanceDivisor = 1;
    gfx::AttributeBinding b = a;
    EXPECT_EQ(b.instanceDivisor, 1u);
    EXPECT_TRUE(a == b);
    b.instanceDivisor = 0;
    EXPECT_FALSE(a == b); // divisor participates in equality (VAO cache key)
}
#endif
```
Add `test/gl/instancing.test.cpp` to the test sources list (find `test/gl/bucket.test.cpp` in `test/CMakeLists.txt` or the bazel `BUILD` and add the new file beside it).

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build-gl-check --target mbgl-test-runner -j && build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.AttributeBindingCarriesDivisor'`
Expected: FAIL to compile — `AttributeBinding` has no `instanceDivisor`.

- [ ] **Step 3: Add the field + equality**

In `src/mbgl/gfx/attribute.hpp`, add to `class AttributeBinding`:
```cpp
    uint32_t instanceDivisor = 0; // 0 = per-vertex, 1 = per-instance (glVertexAttribDivisor)
```
and extend `operator==`:
```cpp
    friend bool operator==(const AttributeBinding& lhs, const AttributeBinding& rhs) {
        return lhs.attribute == rhs.attribute && lhs.vertexStride == rhs.vertexStride &&
               lhs.vertexBufferResource == rhs.vertexBufferResource && lhs.vertexOffset == rhs.vertexOffset &&
               lhs.bufferIndex == rhs.bufferIndex && lhs.instanceDivisor == rhs.instanceDivisor;
    }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-gl-check --target mbgl-test-runner -j && build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.AttributeBindingCarriesDivisor'`
Expected: PASS.

- [ ] **Step 5: Issue `glVertexAttribDivisor` in `VertexAttribute::Set`**

In `src/mbgl/gl/value.cpp`, inside `VertexAttribute::Set`, after the `glVertexAttribPointer(...)` call (still inside the `if (binding && binding->vertexBufferResource)` branch), add:
```cpp
        MBGL_CHECK_ERROR(glVertexAttribDivisor(location, binding->instanceDivisor));
```
and in the `else` branch (the `glDisableVertexAttribArray` path), reset the divisor so a recycled location doesn't keep a stale per-instance divisor:
```cpp
        MBGL_CHECK_ERROR(glVertexAttribDivisor(location, 0));
```

- [ ] **Step 6: Confirm `glVertexAttribDivisor` is exposed**

Run: `grep -rn "glVertexAttribDivisor" src/mbgl/platform/gl_functions.cpp include/mbgl/platform/gl_functions.hpp`
Expected: a declaration/binding exists (it is core in GLES 3.0 / GL 3.3). If MISSING, add it to the gl_functions table mirroring an existing `glVertexAttrib*` entry, and re-run grep to confirm before proceeding.

- [ ] **Step 7: Build to confirm clean**

Run: `cmake --build build-gl-check --target mbgl-render mbgl-test-runner -j`
Expected: clean build.

- [ ] **Step 8: Commit**
```bash
git add src/mbgl/gfx/attribute.hpp src/mbgl/gl/value.cpp src/mbgl/gl/vertex_array.cpp test/gl/instancing.test.cpp test/CMakeLists.txt
git commit -m "feat(gl): instance divisor through AttributeBinding + glVertexAttribDivisor"
```

---

### Task 3: Bucket emits edge-indexed instance data under the gate

**Files:**
- Modify: `src/mbgl/renderer/buckets/fill_extrusion_bucket.hpp` (new layout vertex + accessor under `MLN_GL_FE_INSTANCING`)
- Modify: `src/mbgl/renderer/buckets/fill_extrusion_bucket.cpp` (`addFeature` emission)
- Test: `test/gl/instancing.test.cpp`

**Interfaces:**
- Produces (only when `MLN_GL_FE_INSTANCING`): `FillExtrusionBucket::glEdgeInstances` — a `gfx::VertexVector<GLEdgeInstance>` where
  ```cpp
  struct GLEdgeInstance { // 1 per footprint edge
      int16_t pos0[2];     // edge start (tile coords)
      int16_t pos1[2];     // edge end
      int16_t normal0[3];  // smoothed normal AT pos0 * 2^13  (nP at the start endpoint)
      int16_t normal1[3];  // smoothed normal AT pos1 * 2^13  (nP at the end endpoint)
      uint16_t edgeDistance;
  };
  ```
  **TWO endpoint normals, not one (review P1):** the non-instanced `#else` path computes a distinct smoothed normal at each endpoint (`nP1` at `p1`, `nP2` at `p2` in `fill_extrusion_bucket.cpp:277`) and interpolates them across the wall. A single per-edge normal would flatten curved facades. The shader interpolates `normal0`/`normal1` by `a_pos.x`.
- **Edge-aligned paint stream (review P1):** the data-driven base/height/color binders currently populate per `vertices.elements()` (explicit-wall vertex count), which does NOT match the edge-instance count. The divisor binding indexes by `gl_InstanceID`, so the per-instance base/height/color MUST be a stream with exactly `glEdgeInstances.elements()` entries, one per edge. Produce an edge-aligned paint stream in the bucket (append the evaluated paint value for each edge as it is emitted, or duplicate the per-feature binder value per edge). Do NOT assume the existing binders align. The render layer (Task 10) binds THIS edge-aligned paint data as divisor-1 attributes; verify alignment against the headless golden (Task 10 Step 3).
  - Consumed by Task 10 (render layer builds instance attributes from this) and the GL shaders (Task 6). Edge count = number of emitted `GLEdgeInstance`.

- [ ] **Step 1: Write the failing test**

Append to `test/gl/instancing.test.cpp`:
```cpp
#if MLN_GL_FE_INSTANCING
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/test/stub_geometry_tile_feature.hpp>
// ... build a bucket with a single 4-vertex square ring (closed), call addFeature,
// then assert glEdgeInstances has 4 entries and pos0/pos1 of edge 0 equal the
// first two ring vertices, and normal is unit-ish (non-zero, perpendicular).
TEST(GLInstancing, BucketEmitsOneInstancePerEdge) {
    // (full body: construct PossiblyEvaluated props + a square GeometryCollection,
    //  FillExtrusionBucket bucket(...); bucket.addFeature(feature, geom, {}, {}, 0, canonical);
    //  ASSERT_EQ(bucket.glEdgeInstances.elements(), 4u);
    //  EXPECT_EQ(edge0.pos0, ring[0]); EXPECT_EQ(edge0.pos1, ring[1]);
    //  EXPECT_NE(edge0.normal[0] | edge0.normal[1], 0); )
}
#endif
```
(Model the bucket construction on the `fill_bucket`/`circle_bucket` usage already in `test/gl/bucket.test.cpp`.)

- [ ] **Step 2: Run with the gate on to verify it fails**

Run: `cmake --build build-gl-check --target mbgl-test-runner -j -- -DMLN_GL_FE_INSTANCING=1` (or temporarily set the macro to 1 in `layer_ubo.hpp` for the test build) then `build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.BucketEmitsOneInstancePerEdge'`
Expected: FAIL — `glEdgeInstances` does not exist.

- [ ] **Step 3: Declare the layout vertex + storage**

In `fill_extrusion_bucket.hpp`, under a new `#if MLN_GL_FE_INSTANCING` block (do NOT disturb the existing `MLN_USE_FILL_EXTRUSION_INSTANCING` blocks):
```cpp
#if MLN_GL_FE_INSTANCING
    using GLEdgeInstanceVertex =
        gfx::Vertex<TypeList<attributes::pos,          // pos0 (Short2)
                             attributes::pos1,         // pos1 (Short2) — see attributes.hpp
                             attributes::normal0,      // Short3 smoothed normal at pos0
                             attributes::normal1,      // Short3 smoothed normal at pos1
                             attributes::edgedistance>>; // UShort1
    using GLEdgeInstanceVector = gfx::VertexVector<GLEdgeInstanceVertex>;
    const std::shared_ptr<GLEdgeInstanceVector> sharedGLEdgeInstances =
        std::make_shared<GLEdgeInstanceVector>();
    GLEdgeInstanceVector& glEdgeInstances = *sharedGLEdgeInstances;
#endif
```
Add the supporting `MBGL_DEFINE_ATTRIBUTE`s (`pos1`, `normal`, `edgedistance` if not present) in `src/mbgl/shaders/attributes.hpp` under `#if MLN_GL_FE_INSTANCING` so they don't perturb other backends.

- [ ] **Step 4: Emit one instance per edge in `addFeature`**

In `fill_extrusion_bucket.cpp`, in the ring loop, add an `#if MLN_GL_FE_INSTANCING` branch that, for each edge `e = ring[i] → ring[i+1]` (skipping the ring-closing degenerate), computes BOTH endpoint normals with the EXISTING `blendNormal(perp, adjacentEdge, hasNeighbor)` helper used by the `#else` path — `nP1` at the start endpoint and `nP2` at the end endpoint (same `blendNormal` calls the `#else` path already makes at `fill_extrusion_bucket.cpp:277`) — and appends:
```cpp
const auto pack = [](double v) { return static_cast<int16_t>(std::floor(v * 8192.0)); };
glEdgeInstances.emplace_back(GLEdgeInstanceVertex{
    {ring[i].x, ring[i].y},
    {ring[i + 1].x, ring[i + 1].y},
    { pack(nP0.x), pack(nP0.y), 0 },   // normal at pos0
    { pack(nP1.x), pack(nP1.y), 0 },   // normal at pos1
    { static_cast<uint16_t>(edgeDistance) }});
```
(8192 = 2^13, matching the `#else` packing factor.)

**Edge-aligned paint:** in the SAME branch, append this edge's data-driven base/height/color into edge-aligned paint vertex vectors (one entry per `emplace_back` above), so the paint streams have exactly `glEdgeInstances.elements()` entries. Do NOT rely on the existing vertex-aligned binders (review P1). The simplest correct approach: evaluate the per-feature paint value once and push it for this edge, matching the instance count 1:1.

- [ ] **Step 5: Run the test to verify it passes**

Run: `build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.BucketEmitsOneInstancePerEdge'`
Expected: PASS — 4 instances, endpoints match, normal non-zero.

- [ ] **Step 6: Build with the gate OFF to confirm no leak**

Reset `MLN_GL_FE_INSTANCING` to 0, run `cmake --build build-gl-check --target mbgl-render -j`.
Expected: clean (the `#if` blocks compile out).

- [ ] **Step 7: Commit**
```bash
git add src/mbgl/renderer/buckets/fill_extrusion_bucket.hpp src/mbgl/renderer/buckets/fill_extrusion_bucket.cpp \
        src/mbgl/shaders/attributes.hpp test/gl/instancing.test.cpp
git commit -m "feat(gl): bucket emits edge-indexed FE instances w/ precomputed normals (gated)"
```

---

### Task 4: `Context::drawInstanced`

**Files:**
- Modify: `src/mbgl/gl/context.hpp:83` (declaration)
- Modify: `src/mbgl/gl/context.cpp:767` (definition)

**Interfaces:**
- Produces: `void Context::drawInstanced(const gfx::DrawMode&, std::size_t indexOffset, std::size_t indexLength, std::size_t instanceCount);` — consumed by Task 5 (`DrawableGL::draw`).

- [ ] **Step 1: Declare**

In `src/mbgl/gl/context.hpp` after the existing `draw(...)` declaration:
```cpp
    void drawInstanced(const gfx::DrawMode&, std::size_t indexOffset, std::size_t indexLength,
                       std::size_t instanceCount);
```

- [ ] **Step 2: Define**

In `src/mbgl/gl/context.cpp`, beside `Context::draw`:
```cpp
void Context::drawInstanced(const gfx::DrawMode& drawMode, std::size_t indexOffset,
                            std::size_t indexLength, std::size_t instanceCount) {
    MLN_TRACE_FUNC();
    MLN_TRACE_FUNC_GL();
    switch (drawMode.type) {
        case gfx::DrawModeType::Lines:
        case gfx::DrawModeType::LineLoop:
        case gfx::DrawModeType::LineStrip:
            lineWidth = drawMode.size;
            break;
        default:
            break;
    }
    MBGL_CHECK_ERROR(glDrawElementsInstanced(Enum<gfx::DrawModeType>::to(drawMode.type),
                                             static_cast<GLsizei>(indexLength),
                                             GL_UNSIGNED_SHORT,
                                             reinterpret_cast<GLvoid*>(sizeof(uint16_t) * indexOffset),
                                             static_cast<GLsizei>(instanceCount)));
    stats.numDrawCalls++;
    stats.totalDrawCalls++;
}
```

- [ ] **Step 3: Confirm `glDrawElementsInstanced` is exposed**

Run: `grep -rn "glDrawElementsInstanced" src/mbgl/platform/gl_functions.cpp include/mbgl/platform/gl_functions.hpp`
Expected: present. If missing, add to the gl_functions table mirroring `glDrawElements`, re-grep to confirm.

- [ ] **Step 4: Build**

Run: `cmake --build build-gl-check --target mbgl-render -j`
Expected: clean.

- [ ] **Step 5: Commit**
```bash
git add src/mbgl/gl/context.hpp src/mbgl/gl/context.cpp
git commit -m "feat(gl): Context::drawInstanced (glDrawElementsInstanced)"
```

---

### Task 5: `DrawableGL` instance attributes + instanced draw

**Files:**
- Modify: `include/mbgl/gl/drawable_gl.hpp`, `src/mbgl/gl/drawable_gl_impl.hpp`, `src/mbgl/gl/drawable_gl.cpp`
- Test: `test/gl/instancing.test.cpp`

**Interfaces:**
- Consumes: `Context::drawInstanced` (Task 4); `AttributeBinding::instanceDivisor` (Task 2); `gfx::Drawable::setInstanceAttributes` (already exists, used by the Metal render layer — confirm the base-class setter and the GL drawable stores them).
- Produces: `DrawableGL` builds a second set of bindings from `instanceAttributes` (each with `instanceDivisor = 1`), merges them into the same VAO, computes `instanceCount` = element count of the instance attributes, and in `draw()` calls `context.drawInstanced(..., instanceCount)` when `instanceCount > 0`, else `context.draw(...)`.

- [ ] **Step 1: Write the failing render-ish test**

Append to `test/gl/instancing.test.cpp` a headless test that builds a DrawableGL with a static unit-quad as vertex attributes and a 3-element instance attribute array (divisor 1), uploads via a HeadlessBackend upload pass, draws, and reads back pixels expecting 3 quads rendered (model the HeadlessBackend setup on `test/gl/bucket.test.cpp`). Assert the draw issues an instanced call (e.g. via `context.renderingStats().numDrawCalls == 1` for 3 instances, proving one instanced draw rather than 3).

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-gl-check --target mbgl-test-runner -j && build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.DrawsInstances'`
Expected: FAIL — `DrawableGL` ignores instance attributes / draws non-instanced.

- [ ] **Step 3: Store instance attributes + count on the impl, and dirty the upload**

In `drawable_gl_impl.hpp` add:
```cpp
    gfx::VertexAttributeArrayPtr instanceAttributes;
    AttributeBindingArray instanceAttributeBindings;
    std::vector<gfx::UniqueVertexBufferResource> instanceAttributeBuffers;
    std::size_t instanceCount = 0;
```
In `drawable_gl.hpp` override the base `setInstanceAttributes(gfx::VertexAttributeArrayPtr)` to store into `impl->instanceAttributes`. **Review P2:** the base `setInstanceAttributes` does NOT reset `attributeUpdateTime`, and `upload()` rebuilds bindings + (re)binds the VAO only on vertex-attribute dirtiness / VAO-invalid. So in the GL override, explicitly reset `attributeUpdateTime` (clear the optional) AND invalidate each segment's VAO, so the next `upload()` rebuilds the merged vertex+instance bindings and rebinds the VAO. Without this, instance data set after the first upload is silently ignored.

- [ ] **Step 4: Build instance bindings in `upload()`**

In `DrawableGL::upload`, after building the per-vertex `attributeBindings`, when `impl->instanceAttributes` is set, build a second binding array from it (reuse `uploadPass.buildAttributeBindings` with the instance vertex data), set `binding->instanceDivisor = 1` on every produced binding, store in `impl->instanceAttributeBindings`/`impl->instanceAttributeBuffers`, and set `impl->instanceCount` to the instance attribute element count. Then, when creating each segment's VAO, bind BOTH `attributeBindings` and `instanceAttributeBindings` (extend `VertexArray::bind` or call it with the merged set).

- [ ] **Step 5: Use the instanced draw in `draw()`**

In `DrawableGL::draw`, replace the per-segment draw call:
```cpp
            context.bindVertexArray = glSeg.getVertexArray().getID();
            if (impl->instanceCount > 0) {
                context.drawInstanced(glSeg.getMode(), mlSeg.indexOffset, mlSeg.indexLength, impl->instanceCount);
            } else {
                context.draw(glSeg.getMode(), mlSeg.indexOffset, mlSeg.indexLength);
            }
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build-gl-check/mbgl-test-runner --gtest_filter='GLInstancing.DrawsInstances'`
Expected: PASS — one instanced draw renders 3 quads.

- [ ] **Step 7: Commit**
```bash
git add include/mbgl/gl/drawable_gl.hpp src/mbgl/gl/drawable_gl_impl.hpp src/mbgl/gl/drawable_gl.cpp \
        src/mbgl/gl/vertex_array.cpp src/mbgl/gl/vertex_array.hpp test/gl/instancing.test.cpp
git commit -m "feat(gl): DrawableGL instance attributes + instanced draw path"
```

---

### Task 6: GL `FillExtrusionInstancedShader` (edge-indexed)

> **CRITICAL (review P1): `include/mbgl/shaders/gl/fill_extrusion_instanced.hpp` is GENERATED** ("// Generated code, do not modify this file!" banner). Editing it directly is wrong — it maps to `_empty.glsl` in `shaders/manifest.json:113-116`. Author the real GLSL as `shaders/*.glsl` sources, point the manifest at them, and regenerate via `node shaders/generate_shader_code.mjs`, then commit the regenerated header. (Contrast: `gl/shadow_depth*.hpp` in Task 11 are hand-written — no banner — and edited directly.)

**Files:**
- Create: `shaders/fill_extrusion_instanced.vertex.glsl`, `shaders/fill_extrusion_instanced.fragment.glsl`
- Modify: `shaders/manifest.json:115-116` (point `glsl_vert`/`glsl_frag` from `_empty.glsl` to the new files)
- Regenerate (do not hand-edit): `include/mbgl/shaders/gl/fill_extrusion_instanced.hpp`

**Interfaces:**
- Consumes: per-vertex `a_pos` (static unit quad, divisor 0); per-instance `a_pos0`, `a_pos1`, `a_normal`, `a_edgedistance`, and data-driven `a_base`/`a_height`/`a_color` (divisor 1). UBOs `FillExtrusionDrawableUBO`, `FillExtrusionPropsUBO` (same layouts as `gl/fill_extrusion.hpp`).
- Produces: `ShaderSource<BuiltIn::FillExtrusionInstancedShader, OpenGL>` with non-empty vertex/fragment. The vertex/fragment shading math is copied from `gl/fill_extrusion.hpp` (ambient + directional + vertical-gradient), differing ONLY in geometry assembly (edge-indexed instead of explicit wall vertex).

- [ ] **Step 1: Author the vertex shader (geometry from current instance only)**

Write `shaders/fill_extrusion_instanced.vertex.glsl` with the body below (the generator prepends the `#version`/prelude and the `#pragma`-driven `HAS_UNIFORM_*` plumbing — match how `shaders/fill_extrusion.vertex.glsl` is structured). The static quad vertex `a_pos` has `x ∈ {0,1}` (which endpoint) and `y ∈ {0,1}` (base/height):
```glsl
layout (location = 0) in vec2 a_pos;            // static unit quad: x=endpoint sel, y=base/height sel
layout (location = 1) in vec2 a_pos0;           // instance: edge start (tile coords)
layout (location = 2) in vec2 a_pos1;           // instance: edge end
layout (location = 3) in vec3 a_normal0;        // instance: smoothed normal at pos0 * 2^13
layout (location = 4) in vec3 a_normal1;        // instance: smoothed normal at pos1 * 2^13
layout (location = 5) in highp float a_edgedistance;
out vec4 v_color;

layout (std140) uniform FillExtrusionDrawableUBO {
    highp mat4 u_matrix; highp vec2 u_pixel_coord_upper; highp vec2 u_pixel_coord_lower;
    highp float u_height_factor; highp float u_tile_ratio;
    highp float u_base_t; highp float u_height_t; highp float u_color_t;
    highp float u_pattern_from_t; highp float u_pattern_to_t; lowp float drawable_pad1;
};
layout (std140) uniform FillExtrusionPropsUBO {
    highp vec4 u_color; highp vec3 u_lightcolor; lowp float props_pad1;
    highp vec3 u_lightpos; highp float u_base; highp float u_height;
    highp float u_lightintensity; highp float u_vertical_gradient; highp float u_opacity;
    highp float u_fade; highp float u_from_scale; highp float u_to_scale; lowp float props_pad2;
};
#ifndef HAS_UNIFORM_u_base
layout (location = 6) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 7) in highp vec2 a_height;
#endif
#ifndef HAS_UNIFORM_u_color
layout (location = 8) in highp vec4 a_color;
#endif

void main() {
    #ifndef HAS_UNIFORM_u_base
    highp float base = unpack_mix_vec2(a_base, u_base_t);
    #else
    highp float base = u_base;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = unpack_mix_vec2(a_height, u_height_t);
    #else
    highp float height = u_height;
    #endif
    #ifndef HAS_UNIFORM_u_color
    highp vec4 color = unpack_mix_color(a_color, u_color_t);
    #else
    highp vec4 color = u_color;
    #endif

    base = max(0.0, base);
    height = max(0.0, height);

    bool atStart = (a_pos.x < 0.5);
    vec2 footprint = atStart ? a_pos0 : a_pos1;          // endpoint selected by static-quad x
    float t = a_pos.y;                                   // 0 = base, 1 = height
    gl_Position = u_matrix * vec4(footprint, t > 0.5 ? height : base, 1.0);

    vec3 normal = atStart ? a_normal0 : a_normal1;       // per-endpoint smoothed normal
    float colorvalue = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    v_color = vec4(0.0, 0.0, 0.0, 1.0);
    color += vec4(0.03, 0.03, 0.03, 1.0);
    float directional = clamp(dot(normal / 16384.0, u_lightpos), 0.0, 1.0);
    directional = mix((1.0 - u_lightintensity), max((1.0 - colorvalue + u_lightintensity), 1.0), directional);
    if (normal.y != 0.0) {
        directional *= ((1.0 - u_vertical_gradient) +
            (u_vertical_gradient * clamp((t + base) * pow(height / 150.0, 0.5),
                                         mix(0.7, 0.98, 1.0 - u_lightintensity), 1.0)));
    }
    v_color.r += clamp(color.r * directional * u_lightcolor.r, mix(0.0, 0.3, 1.0 - u_lightcolor.r), 1.0);
    v_color.g += clamp(color.g * directional * u_lightcolor.g, mix(0.0, 0.3, 1.0 - u_lightcolor.g), 1.0);
    v_color.b += clamp(color.b * directional * u_lightcolor.b, mix(0.0, 0.3, 1.0 - u_lightcolor.b), 1.0);
    v_color *= u_opacity;
}
```
(Note: normal is divided by 16384.0 = 2^14 here to match `gl/fill_extrusion.hpp`'s `normal / 16384.0`; the bucket packs at 2^13 — confirm the divisor matches the pack factor during Task 12 visual check and adjust one of them so directional shading magnitude matches the non-instanced path.)

- [ ] **Step 2: Author the fragment shader (identical to non-instanced)**
```glsl
in vec4 v_color;
void main() {
    fragColor = v_color;
#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
```

- [ ] **Step 3: Regenerate the header from the manifest**

Run: `node shaders/generate_shader_code.mjs` (confirm exact invocation from `shaders/CMakeLists.txt` / the script's `--help`; it reads `manifest.json` and writes the `include/mbgl/shaders/{gl,mtl,vulkan}/*.hpp`).
Expected: `git diff include/mbgl/shaders/gl/fill_extrusion_instanced.hpp` now shows the non-empty vertex/fragment matching your GLSL. **Guard:** confirm regeneration did NOT clobber the hand-written `gl/shadow_depth*.hpp` (Task 11) — if the generator touches them, they need the same .glsl-source treatment; record which files the generator owns.

- [ ] **Step 4: Build**

Run: `cmake --build build-gl-check --target mbgl-render -j`
Expected: clean compile of the regenerated header.

- [ ] **Step 5: Commit (source + regenerated output)**
```bash
git add shaders/fill_extrusion_instanced.vertex.glsl shaders/fill_extrusion_instanced.fragment.glsl \
        shaders/manifest.json include/mbgl/shaders/gl/fill_extrusion_instanced.hpp
git commit -m "feat(gl): author FillExtrusionInstancedShader (edge-indexed GLSL ES 3.0)"
```

---

### Task 7: GL `FillExtrusionPatternInstancedShader`

> Same generated-header workflow as Task 6 — author `.glsl` + manifest + regenerate. Do not hand-edit the `.hpp`.

**Files:**
- Create: `shaders/fill_extrusion_pattern_instanced.vertex.glsl`, `shaders/fill_extrusion_pattern_instanced.fragment.glsl`
- Modify: `shaders/manifest.json` (the `FillExtrusionPatternInstancedShader` entry — point its `glsl_vert`/`glsl_frag` away from `_empty.glsl`; if the entry/`header` is missing, add it mirroring the `FillExtrusionInstancedShader` entry)
- Regenerate: `include/mbgl/shaders/gl/fill_extrusion_pattern_instanced.hpp`

**Interfaces:**
- Same edge-indexed geometry (two endpoint normals) as Task 6; pattern sampling math copied from `shaders/fill_extrusion_pattern.{vertex,fragment}.glsl`. Consumes `a_pattern_from`/`a_pattern_to` as per-instance attributes + the pattern texture.

- [ ] **Step 1: Author the vertex GLSL**

Reuse Task 6's geometry assembly (footprint = `a_pos.x<0.5 ? a_pos0 : a_pos1`, `z = t>0.5 ? height : base`, normal = `atStart ? a_normal0 : a_normal1`) and graft the pattern-coordinate computation + lighting from `shaders/fill_extrusion_pattern.vertex.glsl` (the `pos = vec2(edgedistance, z*u_height_factor)` and `get_pattern_pos(...)` lines), using `a_edgedistance`.

- [ ] **Step 2: Author the fragment GLSL**

Copy from `shaders/fill_extrusion_pattern.fragment.glsl` (two-texture mix by `u_fade`, multiplied by lighting) — pattern sampling is identical; only the vertex geometry differed.

- [ ] **Step 3: Update manifest + regenerate**

Edit `shaders/manifest.json`, then `node shaders/generate_shader_code.mjs`. Confirm `git diff` shows the non-empty pattern-instanced header.

- [ ] **Step 4: Build**

Run: `cmake --build build-gl-check --target mbgl-render -j`
Expected: clean.

- [ ] **Step 5: Commit**
```bash
git add shaders/fill_extrusion_pattern_instanced.vertex.glsl shaders/fill_extrusion_pattern_instanced.fragment.glsl \
        shaders/manifest.json include/mbgl/shaders/gl/fill_extrusion_pattern_instanced.hpp
git commit -m "feat(gl): author FillExtrusionPatternInstancedShader (edge-indexed)"
```

---

### Task 8: Register the instanced shaders + split vertex/instance attribute metadata

**Files:**
- Modify: `include/mbgl/shaders/gl/shader_info.hpp` (add `instanceAttributes` to `ShaderInfo` + decls for the 3 instanced shaders)
- Modify: `src/mbgl/shaders/gl/shader_info.cpp` (attribute + instanceAttribute lists)
- Modify: `src/mbgl/shaders/gl/shader_program_gl.cpp` (populate `instanceAttributes` in `create()`)
- Modify: `include/mbgl/shaders/gl/shader_group_gl.hpp` (pass `instanceAttributes` + combined table/id-set to `create()`)
- Modify: `src/mbgl/gl/renderer_backend.cpp:124` (`registerTypes<...>` list)

**Interfaces:**
- Consumes: the shaders authored in Tasks 6/7/11.
- Produces: GL `ShaderProgramGL` whose `getInstanceAttributes()` is non-empty for the instanced shaders, classified per the `instanceAttributes` metadata. `create()` must route an attribute into `vertexAttributes` vs `instanceAttributes` based on which list its id appears in.

- [ ] **Step 1: Add `instanceAttributes` to the GL ShaderInfo struct**

In `shader_info.hpp`, extend the `ShaderInfo<...>` template members with:
```cpp
    static const std::vector<AttributeInfo> instanceAttributes;
```
(default empty for all existing shaders — add a single shared empty definition pattern or define `= {}` per existing shader to keep linkage; mirror how `textures` is handled). Declare specializations for `FillExtrusionInstancedShader`, `FillExtrusionPatternInstancedShader`, `ShadowDepthInstancedShader`.

- [ ] **Step 2: Define attribute + instanceAttribute lists**

In `shader_info.cpp`, for `FillExtrusionInstancedShader`:
```cpp
using FEInstInfo = ShaderInfo<BuiltIn::FillExtrusionInstancedShader, gfx::Backend::Type::OpenGL>;
const std::vector<UniformBlockInfo> FEInstInfo::uniformBlocks = {
    UniformBlockInfo{"FillExtrusionDrawableUBO", idFillExtrusionDrawableUBO},
    UniformBlockInfo{"FillExtrusionPropsUBO", idFillExtrusionPropsUBO},
};
const std::vector<AttributeInfo> FEInstInfo::attributes = {       // divisor 0 (per-vertex)
    AttributeInfo{"a_pos", idFillExtrusionPosVertexAttribute},
};
const std::vector<AttributeInfo> FEInstInfo::instanceAttributes = { // divisor 1
    AttributeInfo{"a_pos0", idFillExtrusionOutlinePosAttribute},
    AttributeInfo{"a_pos1", idFillExtrusionPos1Attribute},          // new id (shader_defines.hpp)
    AttributeInfo{"a_normal0", idFillExtrusionNormal0Attribute},    // new id
    AttributeInfo{"a_normal1", idFillExtrusionNormal1Attribute},    // new id
    AttributeInfo{"a_edgedistance", idFillExtrusionEdDiscardAttribute},
    AttributeInfo{"a_base", idFillExtrusionBaseVertexAttribute},
    AttributeInfo{"a_height", idFillExtrusionHeightVertexAttribute},
    AttributeInfo{"a_color", idFillExtrusionColorVertexAttribute},
};
const std::vector<TextureInfo> FEInstInfo::textures = {};
```
Add new attribute ids `idFillExtrusionPos1Attribute`, `idFillExtrusionNormal0Attribute`, `idFillExtrusionNormal1Attribute` in `include/mbgl/shaders/shader_defines.hpp` (next to the other fill-extrusion ids). The location ordering in this combined table must match the shader `layout(location=N)` (Task 6: a_pos=0, a_pos0=1, a_pos1=2, a_normal0=3, a_normal1=4, a_edgedistance=5, a_base=6, a_height=7, a_color=8). Repeat for the pattern + shadow instanced shaders with their attribute sets.

- [ ] **Step 3: Populate `instanceAttributes` in `ShaderProgramGL::create()` WITHOUT breaking location-indexed lookup**

**Review P1:** `create()` resolves every active attribute via `attributesInfo[location]` (`shader_program_gl.cpp:175`), so `attributesInfo` MUST stay a single table indexed by GL attribute location. Do NOT pass only a subset. Instead:
- Pass a **combined** location-indexed `attributesInfo` (vertex + instance attrs together, as today) PLUS a separate **instance-id set** (the ids listed in `ShaderInfo::instanceAttributes`).
- In the active-attribute loop, after `attributesInfo[location]` gives the id, route the resolved attribute into `instanceAttrs` if its id is in the instance set, else into `attrs`.
- Thread both the combined table and the instance-id set from the shader group. Update `include/mbgl/shaders/gl/shader_group_gl.hpp` (which currently passes only `ShaderInfo::attributes`) to also pass `ShaderInfo::instanceAttributes` and build the combined table + id-set before calling `create()`.
- Pass `attrs` + `instanceAttrs` into the `ShaderProgramGL` constructor (it already stores `vertexAttributes` + `instanceAttributes`).

- [ ] **Step 4: Register the 3 instanced shaders for GL**

In `renderer_backend.cpp`, add to the `registerTypes<...>` pack (under `#if MLN_GL_FE_INSTANCING` so it stays inert when gated off):
```cpp
#if MLN_GL_FE_INSTANCING
                  , shaders::BuiltIn::FillExtrusionInstancedShader
                  , shaders::BuiltIn::FillExtrusionPatternInstancedShader
                  , shaders::BuiltIn::ShadowDepthInstancedShader
#endif
```

- [ ] **Step 5: Build with gate ON**

Set `MLN_GL_FE_INSTANCING=1`, run `cmake --build build-gl-check --target mbgl-render -j`.
Expected: clean — shaders compile + register (any GLSL compile error surfaces here as a runtime log; a smoke `mbgl-render` of any style exercises registration).

- [ ] **Step 6: Reset gate to 0, build, commit**
```bash
git add include/mbgl/shaders/gl/shader_info.hpp src/mbgl/shaders/gl/shader_info.cpp \
        src/mbgl/shaders/gl/shader_program_gl.cpp src/mbgl/gl/renderer_backend.cpp \
        include/mbgl/shaders/shader_defines.hpp
git commit -m "feat(gl): register instanced FE shaders + split vertex/instance attribute metadata"
```

---

### Task 9: Headless render harness for the instanced FE path

**Files:**
- Create: `test/fill-extrusion-instancing/{run-render-test.sh,style.json,expected.png}`

**Interfaces:**
- Produces: a deterministic offline render comparison, modeled on `test/model-layer/run-render-test.sh`, used as the parity gate in Task 12.

- [ ] **Step 1: Create the style + script**

Copy `test/model-layer/run-render-test.sh` to `test/fill-extrusion-instancing/run-render-test.sh`; point it at a `style.json` containing a `fill-extrusion` layer over a fixed building geometry fixture (reuse an existing render-test fill-extrusion fixture if present — grep `fill-extrusion` under `test/fixtures` / `metrics`). Use `build-gl-check` as the default build dir (GL backend):
```bash
BUILD="${1:-$HERE/../../build-gl-check}"
"$BUILD/bin/mbgl-render" --style "file://$HERE/style.json" \
    -y 50.4501 -x 30.5234 -z 16 -p 45 -b 20 -w 512 -h 512 \
    -c /tmp/fe-inst-test-cache.sqlite -o "$OUT"
```

- [ ] **Step 2: Generate the baseline golden with the gate OFF (non-instanced)**

Build `mbgl-render` with `MLN_GL_FE_INSTANCING=0`, run the script, and save the output as `expected.png` (this is the non-instanced reference the instanced path must match).
Run: `cmake --build build-gl-check --target mbgl-render -j && test/fill-extrusion-instancing/run-render-test.sh && cp /tmp/fill-extrusion-instancing-render-test.png test/fill-extrusion-instancing/expected.png`

- [ ] **Step 3: Commit**
```bash
git add test/fill-extrusion-instancing/
git commit -m "test(gl): headless FE-instancing render parity harness + non-instanced golden"
```

---

### Task 10: Render layer builds GL instanced drawables (gated)

**Files:**
- Modify: `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp`, `render_fill_extrusion_layer.hpp`

**Interfaces:**
- Consumes: `bucket.glEdgeInstances` (Task 3); the static unit quad (`RenderStaticData::fillExtrusion{Vertices,TriangleIndices,Segments}`); `DrawableGL` instance attributes (Task 5); the GL instanced shaders (Tasks 6/7).
- Produces: under `#if MLN_GL_FE_INSTANCING`, the render layer builds one instanced color drawable (+ depth drawable) per tile from the static quad + edge instance attributes, instead of the non-instanced explicit-wall drawable. Roof triangles continue via the existing path.

- [ ] **Step 1: Add the gated branch selecting the instanced shader group**

Mirror the structure of the existing `#if MLN_USE_FILL_EXTRUSION_INSTANCING` block (verbatim reference in the spec's research) but under `#if MLN_GL_FE_INSTANCING`, fetching `FillExtrusionInstancedShader`/`FillExtrusionPatternInstancedShader` groups and the static-quad `staticDataVertices/Indices/Segments`.

- [ ] **Step 2: Set up the static-quad vertex attribute + edge-instance attributes**

```cpp
auto vertexAttrs = context.createVertexAttributeArray();
if (const auto& attr = vertexAttrs->set(idFillExtrusionPosVertexAttribute)) {
    attr->setSharedRawData(staticDataVertices, offsetof(FillExtrusionStaticVertex, a1),
                           0, sizeof(FillExtrusionStaticVertex), gfx::AttributeDataType::Short2);
}
auto instanceAttrs = context.createVertexAttributeArray();
if (const auto& a = instanceAttrs->set(idFillExtrusionOutlinePosAttribute))
    a->setSharedRawData(bucket.sharedGLEdgeInstances, offsetof(GLEdgeInstanceVertex, a1) /*pos0*/,
                        0, sizeof(GLEdgeInstanceVertex), gfx::AttributeDataType::Short2);
if (const auto& a = instanceAttrs->set(idFillExtrusionPos1Attribute))
    a->setSharedRawData(bucket.sharedGLEdgeInstances, offsetof(GLEdgeInstanceVertex, a2) /*pos1*/,
                        0, sizeof(GLEdgeInstanceVertex), gfx::AttributeDataType::Short2);
if (const auto& a = instanceAttrs->set(idFillExtrusionNormal0Attribute))
    a->setSharedRawData(bucket.sharedGLEdgeInstances, offsetof(GLEdgeInstanceVertex, a3) /*normal0*/,
                        0, sizeof(GLEdgeInstanceVertex), gfx::AttributeDataType::Short3);
if (const auto& a = instanceAttrs->set(idFillExtrusionNormal1Attribute))
    a->setSharedRawData(bucket.sharedGLEdgeInstances, offsetof(GLEdgeInstanceVertex, a4) /*normal1*/,
                        0, sizeof(GLEdgeInstanceVertex), gfx::AttributeDataType::Short3);
if (const auto& a = instanceAttrs->set(idFillExtrusionEdDiscardAttribute))
    a->setSharedRawData(bucket.sharedGLEdgeInstances, offsetof(GLEdgeInstanceVertex, a5) /*edgeDistance*/,
                        0, sizeof(GLEdgeInstanceVertex), gfx::AttributeDataType::UShort);
// data-driven base/height/color: bind the EDGE-ALIGNED paint streams from Task 3 (one entry per
// edge instance), NOT the vertex-aligned binders — set each as a divisor-1 instance attribute whose
// element count equals glEdgeInstances.elements(). (Review P1: vertex binders are not edge-aligned.)
```
Then `builder->setShader(instancedShader); builder->setRawVertices({}, staticDataVertices->elements(), Short2); builder->setVertexAttributes(vertexAttrs); builder->setInstanceAttributes(instanceAttrs); builder->setSegments(Triangles(), staticDataIndices, staticDataSegments->data(), staticDataSegments->size());` and flush, mirroring the Metal path.

- [ ] **Step 3: Build with gate ON + run the headless parity test**

Run: `cmake --build build-gl-check --target mbgl-render -j` (with `MLN_GL_FE_INSTANCING=1`) then `test/fill-extrusion-instancing/run-render-test.sh`
Expected: PASS — instanced output matches the non-instanced golden within the AE threshold. If walls are wrong (missing/offset/dark), debug the geometry assembly (endpoint selection, normal pack/divisor mismatch from Task 6 Step 1 note) before proceeding.

- [ ] **Step 4: Commit**
```bash
git add src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp src/mbgl/renderer/layers/render_fill_extrusion_layer.hpp
git commit -m "feat(gl): render layer builds edge-instanced FE drawables (gated)"
```

---

### Task 11: GL `ShadowDepthInstancedShader` (real port) + caster wiring

> Unlike Tasks 6/7, `include/mbgl/shaders/gl/shadow_depth_instanced.hpp` is **hand-written** (no "Generated code" banner — verified), matching the hand-written `gl/shadow_depth.hpp`. Edit the `.hpp` directly. **Guard:** confirm `node shaders/generate_shader_code.mjs` (run in Tasks 6/7) does NOT overwrite it — if the manifest lists this shader and the generator owns it, switch to the `.glsl`+manifest workflow instead. Resolve this before authoring.

**Files:**
- Modify: `include/mbgl/shaders/gl/shadow_depth_instanced.hpp` (replace placeholder; hand-written)
- Modify: `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` (gated GL wall-caster build)

**Interfaces:**
- Consumes: the edge-instance attributes (Task 3) + the static quad; `ShadowDepthDrawableUBO`.
- Produces: a GL instanced wall shadow caster matching the non-instanced caster's packed-depth output, so ground shadows reattach to building bases on the instanced path.

- [ ] **Step 1: Author the caster vertex shader (edge-indexed)**

Replace the placeholder vertex `main` so it reconstructs the wall from the current instance (no `u_height`-only collapse):
```glsl
layout (location = 0) in vec2 a_pos;     // static quad
layout (location = 1) in vec2 a_pos0;
layout (location = 2) in vec2 a_pos1;
#ifndef HAS_UNIFORM_u_base
layout (location = 5) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 6) in highp vec2 a_height;
#endif
// UBO + v_depth01 as in the existing placeholder
void main() {
    #ifndef HAS_UNIFORM_u_base
    highp float base = unpack_mix_vec2(a_base, u_base_t);
    #else
    highp float base = u_base;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = unpack_mix_vec2(a_height, u_height_t);
    #else
    highp float height = u_height;
    #endif
    base = max(0.0, base); height = max(0.0, height);
    vec2 footprint = (a_pos.x < 0.5) ? a_pos0 : a_pos1;
    float t = a_pos.y;
    highp vec4 clip = u_light_matrix * vec4(footprint, t > 0.5 ? height : base, 1.0);
    v_depth01 = clip.z / clip.w;
    clip.z = 2.0 * clip.z - clip.w;
    gl_Position = clip;
}
```
Fragment stays the existing `packDepth(v_depth01)`.

- [ ] **Step 2: Wire the gated GL wall caster in the render layer**

Mirror the `#if MLN_DRAWABLE_SHADOWS` instanced wall-caster block (verbatim in spec research) under `#if MLN_GL_FE_INSTANCING`, binding the static quad + edge instance attributes (pos0/pos1 + base/height via the **de-interleaved Float2** handling — keep base/height in their own buffers as the Metal path does) to the `ShadowDepthInstancedShader` group, one drawable per cascade.

- [ ] **Step 3: Build + headless shadow render check**

Run: `cmake --build build-gl-check --target mbgl-render -j` (gate ON) then render a shadows-enabled style and confirm ground shadows reattach to bases (compare against a non-instanced shadow golden you capture the same way as Task 9).

- [ ] **Step 4: Commit**
```bash
git add include/mbgl/shaders/gl/shadow_depth_instanced.hpp src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp
git commit -m "feat(gl): real ShadowDepthInstancedShader + gated GL wall caster"
```

---

### Task 12: On-device parity + A/B perf on the Honor

**Files:** none (verification + packaging only)

**Interfaces:** Consumes the full gated path (Tasks 0–11). Produces the go/no-go evidence for the collapse follow-up.

- [ ] **Step 1: Build the instanced AAR**

With `MLN_GL_FE_INSTANCING=1`, run `./gradlew :MapLibreAndroidTestApp:assembleOpenglDebug` (arm64). Then `rm -rf ~/.gradle/caches/modules-2/files-2.1/org.maplibre.gl`, republish to `~/.m2`, rebuild HataHub (`EXPO_USE_COMMUNITY_AUTOLINKING=1 npx expo run:android`).

- [ ] **Step 2: Visual parity on-device**

On the Honor, at z16 + pitch, confirm vs the current shipping build: building walls present + correctly lit, smooth curved facades (Парус / Римарська "20"), no faceted regression, selection highlight intact, shadows reattached, and **no z-fighting/stipple at close zoom** (GLES depth precision). Capture screenshots `~/Work/maplibre-model-layer-qa/gl-instancing/`.

- [ ] **Step 3: A/B perf (SurfaceFlinger latency)**

Benchmark per the 2026-06-13 method: clear the ring, ONE continuous pan, read `dumpsys SurfaceFlinger --latency <BLAST layer>` immediately. Compare instanced build vs current non-instanced build at z16-with-buildings. Record present cadence + any tile-load hitch delta.

- [ ] **Step 4: Record results**

Write findings to `docs/superpowers/plans/2026-06-17-android-gl-fill-extrusion-instancing-m1-results.md` (parity pass/fail per item, perf deltas, screenshots path). This gates whether the collapse (delete `#else`, default gate on) follow-up proceeds.

- [ ] **Step 5: Commit**
```bash
git add docs/superpowers/plans/2026-06-17-android-gl-fill-extrusion-instancing-m1-results.md
git commit -m "docs: GL FE-instancing M1 on-device parity + perf results"
```

---

## Out of scope (follow-ups)

- **Collapse commit:** flip `MLN_GL_FE_INSTANCING` default to 1 + delete the GLES `#else` wall-quad bucket/render path — only after Task 12 sign-off.
- **Cadence sub-track (60fps):** separate Plan 2 (render-thread scheduling) — see spec §4.
- **Vulkan backend, iOS/Metal changes, upstream PRs.**

## Execution deviations (recorded during inline execution 2026-06-18)

- **Pre-req fix (commit a98293e6):** `test/gl/gl_functions.test.cpp` asserted `glInvalidate{Sub,}Framebuffer != nullptr` unconditionally, but those are defined only for iOS/sim on Apple (`platform/darwin/src/gl_functions.cpp`); macOS desktop GL omits them, so `mbgl-test-runner` would not link in `build-gl-check`. Guarded the asserts to match the production platform guard. This unblocks the entire headless TDD surface.
- **Task 2:** `vertex_array.cpp` needed NO change — the divisor rides through `AttributeBinding` into `VertexAttribute::Set` (the field + the `value.cpp` call are sufficient). The plan over-listed it.
- **Task 3 DATA MODEL — adopt Metal's "one instance per outline vertex" instead of "one instance per edge + separate edge-aligned paint stream":** Reading `render_fill_extrusion_layer.cpp`'s Metal path showed the walls are drawn with **instance count == `bucket.sharedVertices.elements()`** and base/height/color bound via `instanceAttrs->readDataDrivenPaintProperties(binders, …)`, whose vectors are sized to `vertices.elements()` — so paint aligns 1:1 with instances **for free**. Mirroring that (one `glEdgeInstance` per outline/roof vertex, in lockstep with `vertices`) lets the GL path reuse `readDataDrivenPaintProperties` verbatim — NO separate edge-aligned paint stream, eliminating the review-P1 misalignment risk entirely. The only GLES-specific addition is that each instance carries `pos1`/`normal0`/`normal1` precomputed (GLES can't fetch `outline[instanceID+1]`). The last vertex of each ring emits a **degenerate** instance (`pos1 = pos0` → zero-area wall → no fragments), so no discard attribute is needed. `glEdgeInstances` therefore has exactly one entry per `vertices` entry.
- **Shader generation = regenerate-in-place + restore hand-maintained headers (Tasks 6/7):** `generate_shader_code.mjs` rewrites `gl/<header>.hpp` for EVERY manifest entry, including the `_empty.glsl`-mapped ones — and `gl/shadow_depth.hpp`, `gl/shadow_depth_instanced.hpp`, `gl/fill_extrusion_shadow.hpp`, `gl/ground_shadow.hpp` are HAND-WRITTEN (no banner) and `gl/model_bloom.hpp` is hand-maintained (regen produces a different result than the committed one). The `--out`/`-o` flag is BROKEN (silently writes to the real tree regardless), so a temp-dir approach is impossible. Working procedure: run `node shaders/generate_shader_code.mjs` in place, then `git checkout HEAD --` the hand-maintained set — `gl/{shadow_depth,shadow_depth_instanced,fill_extrusion_shadow,ground_shadow,model_bloom}.hpp`, `shader_manifest.hpp`, `src/mbgl/shaders/shader_source.cpp` — keeping ONLY the regenerated target (`fill_extrusion_instanced.hpp` / `fill_extrusion_pattern_instanced.hpp`). The `BuiltIn` enum is unchanged (these shaders already existed as placeholders). Task 11 must edit `shadow_depth_instanced.hpp` AFTER any generator run (and never run the generator after).
- **Task 8 instance classification = free-function template, not a ShaderInfo member:** Rather than add a `static const std::vector<AttributeInfo> instanceAttributes` member to all ~40 `ShaderInfo` specializations (and `= {}` definitions), added `template <BuiltIn T> const std::vector<AttributeInfo>& shaders::instanceAttributes()` (default empty, specialized for the 2 FE instanced shaders). `shader_group_gl.hpp` passes it to `create()`. `create()` keeps `attributesInfo` as the single combined location-indexed table (assert + resolution unchanged) and routes each active attribute into the new `instanceAttrs` array iff its id is in the instance-id set; a new `ShaderProgramGL` constructor takes both arrays. New attribute ids added under `#elif MLN_GL_FE_INSTANCING` in `shader_defines.hpp`: `idFillExtrusionPos1Attribute`, `idFillExtrusionNormal0Attribute`, `idFillExtrusionNormal1Attribute`, `idFillExtrusionEdgeDistanceAttribute` (+ reuse `idFillExtrusionOutlinePosAttribute` for pos0, keep `idFillExtrusionNormalEdVertexAttribute` for the roof). `idFillExtrusionEdgeDistanceAttribute` is used instead of the plan's Metal-only `idFillExtrusionEdDiscardAttribute`.
- **ShadowDepthInstancedShader registration moved from Task 8 to Task 11:** its GLSL/attributes are authored in Task 11, so registering it + defining its ShaderInfo belongs there. Task 8 registers only the 2 FE instanced shaders.
- **Task 5 test relocated:** A standalone DrawableGL instanced-draw test needs a full pipeline (shader program, uniform buffers, a render pass, framebuffer readback + pixel asserts) — no lightweight harness exists, and it would largely duplicate the render-parity gate. The functional verification of the instanced draw path (merged VAO, divisor-1 bindings, `drawInstanced`) is therefore relocated to the headless render-parity test (Task 9/10), which drives the exact same path with the real shader + golden comparison. `setInstanceAttributes` is NOT overridden (the base setter stores into the protected `instanceAttributes` member; DrawableGL reads it in `upload()` and handles dirty/VAO-invalidation there, mirroring the Metal drawable).
- **Normal pack factor reconciled to 2^14 (16384):** the non-instanced `layoutVertex` packs normals at `ny*16384` and the shader divides by `16384`. The bucket packs `normal0`/`normal1` at `floor(n*16384)` and the GL instanced shader divides by `16384.0` — exact magnitude parity (resolves the previously-flagged 2^13-vs-2^14 open detail). The static-quad `a_pos.y` carries base/top, so no LSB is stolen for `t`.

## ⚠️ Environmental blocker: headless GL rendering impossible on macOS (discovered Task 9)

`build-gl-check`'s `mbgl-render` on this Mac gets `GL_VERSION: 2.1 Metal - 90.5` — the CGL
headless backend (`platform/darwin/src/headless_backend_cgl.mm`) requests
`kCGLOGLPVersion_Legacy` (GL 2.1). The mbgl GL shaders are all `#version 300 es` (GLES 3.0), so
they fail to compile (`version '300' is not supported`) and `mbgl-render` aborts — for EVERY
shader, not just the instanced ones (reproduced on a stock fill-extrusion render). Apple desktop
GL has no GLES profile, so even switching to `kCGLOGLPVersion_3_2_Core` (GL 4.1) would not accept
`#version 300 es`. **Headless GL render-parity (Tasks 9 golden / 10 / 11 render checks) cannot run
on macOS.** It can run on a GLES environment (Android device, or a Linux EGL+Mesa/SwiftShader
headless build / CI). Unit tests + C++ build (gate on AND off) DO work on macOS and are green.

Consequence for inline execution: Tasks 9–11 are authored + compile-verified (gate on/off) on
macOS; render-correctness (GLSL compile + geometry/winding/parity) is verified on-device (Task 12,
Android), which is the milestone's real target anyway. The Task 9 harness (style.json +
run-render-test.sh) is committed for use on a GLES environment; the golden must be generated there.

## Self-Review notes

- Spec coverage: Layer 0 → Task 0; Layer 1 → Tasks 2,4,5; Layer 2 → Tasks 6,7,8,11; Layer 3 → Tasks 3,10; Layer 4 gate → Task 1 (used by 3,8,10,11); verification → Tasks 9,12. Cadence is intentionally Plan 2.
- Type consistency: `glEdgeInstances`/`sharedGLEdgeInstances`, `GLEdgeInstanceVertex` (fields a1=pos0, a2=pos1, a3=normal0, a4=normal1, a5=edgeDistance), `instanceDivisor`, `Context::drawInstanced`, `DrawableGL::impl->instanceCount`, ids `idFillExtrusionPos1Attribute`/`idFillExtrusionNormal0Attribute`/`idFillExtrusionNormal1Attribute` — used consistently across tasks. Shader attribute locations (0..8) match the combined `ShaderInfo` table (Task 8).
- Review-incorporated (2026-06-17): two endpoint normals (P1), edge-aligned paint stream (P1), generated-header workflow for Tasks 6/7 vs hand-written Task 11 (P1), combined location-indexed `ShaderInfo` + instance-id set + `shader_group_gl.hpp` (P1), instance dirty/VAO invalidation (P2).
- Known open detail flagged inline for execution: normal pack factor (2^13 in bucket) vs shader divisor (`/16384.0`=2^14 in `gl/fill_extrusion.hpp`) — reconcile during Task 10 Step 3 / Task 6 Step 1 against the headless golden.
