# Directional Building Shadows — Root-Cause Analysis & Open Issue

**Branch:** `shadows/s1-shadowmap-ios`  ·  **Backend:** Metal (iOS)  ·  **Date:** 2026-06-15

This document exists because we have been changing the shadow code repeatedly while the
user keeps seeing the **same two failures** on device. The goal here is to stop guessing,
state the problem precisely, and identify the *root cause* — not treat symptoms.

---

## 0. Mapbox comparison update (2026-06-15)

`~/Work/mapbox-maps-ios` is a Swift SDK wrapper around the binary `mapbox-core-maps-ios`
dependency; it does **not** contain the renderer/shader implementation. So the exact Mapbox
shadow-pass internals cannot be verified from that checkout alone.

What the public API *does* verify:

- `DirectionalLight` owns shadow enable/intensity and the ground insertion point:
  `cast-shadows`, `shadow-intensity`, `shadow-draw-before-layer`.
- `FillExtrusionLayer` only participates with `fill-extrusion-cast-shadows` (default true).
- `ModelLayer` has both `model-cast-shadows` and `model-receive-shadows`.

That is an architectural mismatch with the current MapLibre branch. Our implementation owns a
separate `ShadowMap`, caster pass, and ground receiver inside **each** `RenderFillExtrusionLayer`.
That can pass single-layer synthetic/headless tests while still being the wrong render graph for
HataHub's real style, which has multiple extrusion layers (`building-3d`, hover/listing/selected,
plus runtime highlight layers) and model layers. A Mapbox-style rewrite should make shadows
light-/renderer-owned: collect all shadow-casting 3D drawables into one light pass, render the
ground shadow once at a style-controlled insertion point, and let participating receiver layers
sample the same light-owned shadow texture.

Also verified live on 2026-06-15: `https://map.hatahub.com.ua/style/light.json` still serves
legacy `light.anchor: "viewport"` (intensity `0.22`). A viewport-anchored legacy light rotates
with camera bearing, so the HataHub app is not currently loading the world-fixed sun required by
the user's spec.

---

## 1. Symptoms (as reported by the user, repeatedly and consistently)

On the HataHub iOS app, at a tilted 3D view of central Kyiv:

1. **Shadows render only in roughly the bottom half of the screen.** The far field (upper
   part of a pitched view) has buildings but little or no cast shadow.
2. **Shadows are not aligned with the buildings** that should cast them.
3. (Earlier framing of the same complaint) shadows appeared to **depend on / move with the
   camera** — they should be world-anchored like a classic directional light.
4. The user reports the result is **essentially unchanged across every fix attempted so far.**

The user's mental model is correct and is the spec: *point a static directional light at the
geometry; the shadow is fixed in world space and does not move as the camera rotates or tilts;
buildings cast onto the ground and onto each other.*

---

## 2. THE CENTRAL UNRESOLVED DISCREPANCY (read this first)

Every verification artifact **I** have produced looks correct, yet the user consistently sees
the failure. Specifically:

| Artifact | Result |
|---|---|
| Headless `mbgl-render`, real Kyiv style, z16/p60, dense framing | Full-coverage, grounded shadows (`/tmp/dense_p60.png`) |
| Headless synthetic 3-building scene, oblique sun, p50 | Perfect base-attached shadows + inter-building occlusion (`/tmp/synth3_p50.png`) |
| Demo MapLibre app (statically-linked engine) on the simulator, z16/p60 | Full-coverage directional shadows (`/tmp/demo_shadow.png`) |
| Unit tests (bearing-invariance, alignment, depth pack) | 14/14 green |
| **HataHub app on the user's device** | **Bottom-half + misaligned** |

**This gap is the actual blocker.** As long as my captures and the user's screen disagree,
any further change to the frustum math is a shot in the dark. Three classes of explanation:

- **(A) Different build — now the leading explanation.** The build the user is viewing may not be
  the one I verified: a stale/failed dynamic-framework swap, a cached JS bundle, or they are looking
  at the HataHub app while I verified the demo app. *Must be ruled out first* — and it is now the
  top suspect because (B) was tested and largely refuted.
- **(B) A device-only factor absent from my captures.** I **tested camera padding** (the strongest
  such candidate) headless and it did **not** reproduce the failure (Section 5.1). Remaining: device
  DPR / `getSize()` units, the app's exact zoom/pitch/bearing, viewport-anchored sun with non-zero
  bearing, the RN `MapView` wrapper.
- **(C) I am mis-reading my own screenshots.** The user has flagged this before ("you are lying
  to yourself"). Captures must be judged by looking at pixels, and we must look at the *same*
  pixels.

---

## 3. How the shadow system works (so the analysis is grounded)

Shadow mapping, one shadow map, one caster pass (Mapbox-style, minus cascades):

1. **Light frustum** (`computeWorldToLightClip`, `shadow_tweakers.cpp`): build `worldToLightClip`
   = `ortho(AABB) · lightView(sunDir)`. Cached once per frame and shared by the caster and all
   receivers (so they register exactly).
2. **Caster pass** (`ShadowDepthShader`, into a depth-capable `RenderTarget`): render the
   building geometry from the sun's POV; each fragment packs `ndc.z` into RGBA8. Nearest-to-light
   wins via the real depth attachment (`offscreen_texture.cpp` makes+binds a `Depth32Float`).
3. **Receivers** sample the map at `uv = ndc.xy·0.5+0.5` and PCF-compare depth:
   - Ground: flat per-tile `z=0` quads (`GroundShadowShader`).
   - Buildings: the visible fill-extrusion, shadow-receiving variant (`FillExtrusionShadowShader`).

Caster `light_matrix` and receiver `light_matrix` are both `worldToLightClip · matrixFor(tile)`,
from the same cached `worldToLightClip` — so **alignment is guaranteed at the projection level**
(confirmed by the synthetic test). Any *real* misalignment must come from an input that differs
between caster and receiver, or from the frustum being centered on the wrong world point.

---

## 4. What has been tried (honest log — most of it treated symptoms)

| Attempt | Intent | Outcome |
|---|---|---|
| `pitchShadowFade` (fade shadows out below ~12° pitch) | Hide top-down clutter | Masked a real pitch-0 bug; **removed** |
| View-frustum-fit with `maxDist=2600` clamp | Cover the pitched view | Coupled coverage to camera + clamp caused far cutoff; **replaced** |
| Fixed-radius frustum (`1.5·screenExtent`) | Pitch-invariance + stability | Pitch-invariant, but user still saw bottom-half |
| Bearing-invariant reach radius (center-column far reach) | Cover far field + rotation-stable | Current; headless full coverage; **user still sees bottom-half** |
| Receiver depth-range guard (`ndc.z ∈ [0,1]`) | Kill phantom far-plane shadow | Correct + necessary; fixed a pitch-0 diagonal |
| Removed pitch/depth fades | World-anchored strength | Correct |

All of the above are GREEN in unit tests and headless renders. **None changed what the user sees.**
That pattern itself points at Section 2 (A)/(B): we are not exercising the failing code path.

---

## 5. Candidate root causes, ranked by their ability to explain the discrepancy

### 5.1 Camera padding — TESTED, does NOT reproduce the failure (downgraded)
HataHub sets `CAMERA_PADDING = { bottom: 130, left: 24, right: 24, top: 88 }`
(`apps/mobile/components/listings/listing-map-screen.tsx:160`). `computeWorldToLightClip` derives
its focal point from the **geometric** screen center (`screenPixelToWorld(0.5·width, 0.5·height)`),
ignoring padding, so I expected the padded device camera to shift the frustum and cause the
bottom-half.

**Test (2026-06-15):** rendered z16/p60 headless with `MLN_RENDER_PADDING="88,24,130,24"` vs no
padding (`/tmp/pad_on.png` vs `/tmp/pad_off.png`). The two are nearly identical — padding shifts the
framing slightly but does **not** produce the dramatic "bottom-half + clean top." So camera padding
is **not** the dominant cause. (It is still a minor correctness issue worth fixing — the frustum
should center on the real look-at — but it does not explain the user's report.)

**The significant result of this test is the negative one:** even reproducing the device's padded
camera, headless still looks correct. I cannot reproduce the user's failure with `mbgl-render` at
all. That pushes the explanation firmly toward Section 5.2 (build mismatch) or a device-only
rendering/scaling factor — i.e. **we are not running the same code path or the same pixels.**

### 5.2 Different / stale build on device (must rule out FIRST)
The HataHub deploy swaps the dynamic `MapLibre.framework` binary inside the installed `.app` and
ad-hoc re-signs it. If the swap or relaunch didn't actually take, the user sees the old engine.
The demo app (static link) is unambiguous; HataHub is not. We have no on-screen build stamp to
confirm which binary is live.

### 5.3 `getSize()` units / device DPR
`screenExtent` and the reach are computed from `state.getSize()`. If that returns framebuffer
pixels (≈3× points on this device) where the math assumes points, the radius is mis-scaled.
(Would tend to over-cover, not under-cover — lower probability for "bottom half", but it would
break the headless↔device correspondence.)

### 5.4 Viewport-anchored sun + non-zero bearing
The live R2 style (`v7`) has `light.anchor: "viewport"`, so the sun rotates with the camera
bearing. If the user has rotated the map, shadows point in a screen-relative direction →
misaligned. (The staged `anchor: "map"` fix is not deployed.) Only relevant if bearing ≠ 0.

### 5.5 Height ramp + overscaled far tiles
The style interpolates `fill-extrusion-height` from 0 at z15 to full at z16. At ~z15 and on the
lower-zoom overscaled tiles that cover the far field at pitch, buildings are short → faint
shadows → reads as "bottom half." This is a style characteristic, not an engine bug, but it
*contributes* to the perception at the zooms the user uses.

### 5.6 Caster vs visible-building height divergence (misalignment)
If the caster's per-vertex height (`height_t` interpolation) ever differs from the visible
building's, the shadow is cast from the wrong height and detaches. Believed fixed, but not
re-verified at mid-ramp fractional zooms on real tiles.

### 5.7 Single shadow map is fundamentally limited at steep pitch (quality, not "bottom half")
A single map trades near resolution against far coverage. Mapbox uses **2 cascades**. We proved
coverage *can* span the visible screen with one map, so this is not the cause of "bottom half",
but it is the right answer for crisp far-field shadows.

---

## 6. Diagnostic protocol — do THIS before any more code changes

The objective is to make the device and a headless render show the **same** thing, so the bug is
reproducible outside the device.

1. **Confirm the live build.** Put a visible version/build stamp on the map screen (and/or a tiny
   on-screen debug overlay), so we know the user is viewing the engine we think they are. The dev
   build's JS logs do not reach Metro/logcat, so this must be on-screen.
2. **Capture the exact failing camera.** Read off (or overlay) the device's `zoom`, `pitch`,
   `bearing`, `CAMERA_PADDING`, and `getSize()` at the moment the bug is visible.
3. **Reproduce headless with those exact numbers, including padding.** Use `mbgl-render` with the
   `MLN_RENDER_PADDING="top,right,bottom,left"` harness set to `88,24,130,24` and the captured
   zoom/pitch/bearing. **If the bug now appears headless → it is padding/params (5.1/5.3).** If it
   still looks correct headless → it is the build/integration (5.2) or sun anchor (5.4).
4. **Dump the shadow map on-device** (`MLN_SHADOW_DUMP`) to confirm casters are written where
   expected, and add the frustum center/radius to an on-screen overlay to see if it is offset from
   the look-at.

Only after one of these reproduces the user's exact view should we change code.

---

## 7. Is this a fundamental MapLibre limitation? — No.

- The depth attachment is real and bound (`offscreen_texture.cpp:32-77`); inter-building
  occlusion works.
- The orthographic light projection is correct; caster and receiver register exactly; the
  synthetic scene and the demo app render correct, attached, inter-building shadows.
- Mapbox GL JS v3 ships this exact feature on the same renderer lineage.

The remaining issues are an **integration/parameterization bug** (most likely camera padding,
Section 5.1) plus **quality tuning** (intensity, shadow-map resolution, optionally cascades for the
far field). None of these require abandoning the approach.

---

## 8. Recommended path

1. **Reconcile the discrepancy (Section 6).** This is the real work; everything else is blocked on it.
2. **If padding is confirmed:** feed the camera padding into `computeWorldToLightClip` so the
   frustum centers on the *actual* look-at (the center of the padded viewport), not the geometric
   screen center; recompute the far reach about that point.
3. **Deploy the staged `anchor: "map"` style** so the sun is world-fixed under rotation (production
   R2 change — needs explicit go-ahead).
4. **Tune** intensity (~0.35–0.40) and shadow-map size (1024 → 2048).
5. **If far-field crispness is still wanted:** add a second cascade (Mapbox's approach).

---

## 9. Key files

- `src/mbgl/renderer/shadows/shadow_tweakers.cpp` — `computeWorldToLightClip`, the three tweakers.
- `src/mbgl/renderer/shadows/shadow_frustum.cpp` — `lightView` / `fit` / `texelSnap`.
- `include/mbgl/shaders/mtl/{shadow_depth,ground_shadow,fill_extrusion_shadow}.hpp` — caster + receivers.
- `src/mbgl/renderer/layers/render_fill_extrusion_layer.cpp` — caster/ground-quad creation, cull, depth.
- `test/renderer/shadow_frustum.test.cpp` — invariance + alignment unit tests.
- `apps/mobile/components/listings/listing-map-screen.tsx` — `CAMERA_PADDING`, the app's camera.
- Debug envs: `MLN_SHADOW_DUMP`, `MLN_SHADOW_INTENSITY` (>1.5 = ground viz), `MLN_SHADOW_CULL`,
  `MLN_SHADOW_DBG`, `MLN_SHADOW_MAP_SIZE`, `MLN_RENDER_PADDING` (headless padding harness).
