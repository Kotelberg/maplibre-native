# GL Fill-Extrusion Instancing — M1 On-Device Results

**Date:** 2026-06-18
**Device:** Honor NLA-LX2P (arm64-v8a), connected via adb (`A99JBB6116111757`)
**Build:** `MapLibreAndroidTestApp` `installOpenglDebug`, `MLN_GL_FE_INSTANCING=1` (gate ON), arm64 only.
**Verified by:** Claude, driving the device over adb (the dev Mac cannot render GLES — see plan: macOS CGL = GL 2.1, rejects `#version 300 es`).

## Build / runtime

- ✅ Native arm64 CMake build with the gate ON **compiles + links** (the whole instanced path).
- ✅ App installs + runs; **no crash**, no `FATAL`/`SIGSEGV`/`SIGABRT` in logcat.
- ✅ **No shader-compile errors** — specifically none of the `version '300' is not supported` /
  `'layout' : syntax error` failures the macOS GL 2.1 context produced. The `#version 300 es`
  instanced shaders (FillExtrusion / FillExtrusionPattern / ShadowDepth instanced) compile on the
  Honor's real GLES driver.
- Note: mbgl `Info`-level logs (the Task-0 `GL_VERSION` line, the pre-existing `GPU Identifier`)
  are filtered out by this build's log level, so their absence in logcat is expected, not a defect.
  Shader-compile failures log at ERROR and none appeared.

## Visual parity (screenshots in `~/Work/maplibre-model-layer-qa/gl-instancing/`)

1. **`FillExtrusionActivity`** (preset camera, tilt 45°, zoom 18 — Utrecht):
   - Red Dom Tower (local GeoJSON, **uniform** height) renders as a proper instanced extrusion:
     vertical walls + lit top. → edge-indexed wall geometry correct on the uniform path.
   - Background openfreemap-liberty city buildings (**data-driven** `render_height`) render at
     varied heights with correct depth. → **data-driven instance-attribute path** correct.
   - Correct directional lighting/shading; no z-fighting, no missing/offset walls, no corruption.

2. **`ShadowDemoActivity`** (preset camera, tilt 60°, zoom 17.5 — Kyiv / Maidan; `cast-shadows`
   style, shadow-intensity 0.6):
   - 3D buildings render with walls + varied heights; cast-shadow style (caster + receiver +
     the **Task 11 instanced wall caster**) runs with no shader/GL errors.
   - Ground darkening reads as grounded at building bases (no obvious detached/floating shadow).

**Conclusion:** the GL fill-extrusion instancing path renders correctly on a real Android GLES
device — uniform + data-driven fill-extrusion, lighting, and the instanced shadow caster all work.
The big post-macOS unknown (do the `#version 300 es` instanced shaders compile + render on GLES?)
is **resolved: yes**.

## A/B perf — BLOCKED on this device (tooling, not the feature)

- `dumpsys SurfaceFlinger --latency <BLAST layer>` returns empty — this HiHonor OEM build strips
  the SurfaceFlinger latency interface.
- `dumpsys gfxinfo <pkg>` reports 0 frames — the MapLibre map renders through its own GL
  `SurfaceView`, bypassing Android's HWUI pipeline, so gfxinfo can't see its frames.
- Frame-timing A/B (gate-ON vs gate-OFF) therefore needs either a `perfetto`/systrace capture
  (OEM-resistant) or a device whose SurfaceFlinger latency is enabled. **Not yet measured.**

## Outstanding (follow-ups)

- **Perf A/B (the "buttery smooth" goal):** measure gate-ON vs gate-OFF via perfetto, or on a
  device with SurfaceFlinger latency, or by direct feel. Tile-load hitch + memory deltas are the
  expected wins (fewer vertices uploaded per tile).
- **Close-zoom depth-precision check** (GLES depth < Metal): inspect for z-fighting/stipple on the
  HataHub buildings at very close zoom.
- **Smooth curved-facade check:** the test-app buildings are mostly rectangular; verify the
  `blendNormal` smooth facades on a curved building (e.g. Парус / Римарська "20") in HataHub.
- **Real-product run:** rebuild HataHub against the gate-ON AAR and verify its actual building view.
- **Collapse follow-up** (separate): flip the gate default to 1 + delete the `#else` GLES wall path,
  only after the perf A/B + close-zoom sign-off.

## GL cast-shadow regression — found + fixed (2026-06-18, on-device)

While verifying on-device the user reported GL cast-shadows broken (flat-grey buildings, no anchored
ground shadows). **Bisected on-device** (build → ShadowDemoActivity screenshot → ImageMagick pixel
diff as the reliable signal — the shadows are subtle enough to misjudge by eye):
- Pre-instancing baseline `15e22b7` == broken == gate-OFF == gate-ON → **NOT the instancing work**
  (triple-confirmed; the instancing A/B is pixel-identical bar wall-edge AA).
- `4758809` (one commit earlier) == shadows working. Regression = **`0a81f793`** (the Metal SIGABRT
  fix), which is an *ancestor* of the instancing branch.

**Root cause:** `ShadowMap::ensure` eagerly calls `renderTarget->getTexture()->create()` so unrendered
Metal cascades hold a valid texture (`mtl::Texture2D::bind` asserts `!textureDirty`). `mtl::Texture2D
::create()` is idempotent, but `gl::Texture2D::create() -> allocateTexture()` UNCONDITIONALLY
allocated a new GL texture every call → eager create made texture #1, then `OffscreenTextureResource
::bind()` (first caster render) made #2 + attached it to the FBO, orphaning #1 — which the receiver had
already captured → GL receiver samples an empty texture → no cast shadows. The same orphaning was the
"over-shadow latch after zoom-out round-trip" that had kept multi-cascade disabled on Android.

**Fix (commit 8a6d9a4):** make `gl::Texture2D::create()` idempotent (guard `allocateTexture` on
`!texture`, matching the mtl contract). Resizes go via `upload()` (explicit realloc), unaffected.
Single-cascade GL shadows restored — pixel-diff ≈ working `4758809`, far from broken. Headless GL
build + 10 GL unit tests still green.

**2-pass / multi-cascade on GL — renders, but a cold-start bug remains (commits d48e6fe, efd5532a8):**
with the orphaning fixed, the multi-cascade path RENDERS correctly on OpenGL, and the persistent
zoom-round-trip *latch* is gone (the idempotent-create fix cured it: WARM before/after pixel diff =
3696, camera noise only). The pitch gate (`activeShadowCascadeCount`: 1 flat / 2 when pitch ≥ 20°)
gives the crisp near cascade only when buildings are viewed at an angle — matching Metal's "second
pass only when pitched."

**But a COLD-START over-shadow survives, 2-cascade only.** Repro (Honor, tilt 60°): kill the app →
cold-launch ShadowDemoActivity → zoom out ~2 steps → buildings are over-darkened (cold mean 0.889 vs
warm 0.904 at the same camera). It clears after one zoom in/out cycle. This is NOT the latch (that's
fixed) and NOT a caster/receiver height desync (both use `interpolationFactor(zoom)` per frame); it
is a multi-shadow-texture **state-init** issue — the GPU-frame-capture class flagged in
`project_maplibre_shadows_atmosphere` item 7f. Single-cascade is 100% clean across cold start +
round-trip.

**Decision: GL ships single-cascade by default** (commit efd5532a8 reverts the d48e6fe default from
2→1). Single is correct, blackout-free, and ~2× cheaper (one caster pass + one receiver sample). The
2-cascade path stays compiled and reachable via `MLN_SHADOW_CASCADE_COUNT=2` for the continued
cold-start hunt (needs RenderDoc/GPU capture on device to pin). Metal keeps its shipped 2-cascade
default; Vulkan stays single (separate issues). QA screenshots 04–07 in
`~/Work/maplibre-model-layer-qa/gl-instancing/`.
