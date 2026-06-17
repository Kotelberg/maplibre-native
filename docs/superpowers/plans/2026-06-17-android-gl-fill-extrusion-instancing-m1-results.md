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
