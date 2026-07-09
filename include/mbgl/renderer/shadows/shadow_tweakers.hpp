#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/vectors.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mbgl {

class PaintParameters;
class LayerGroupBase;
class TransformState;

struct ShadowFrustumState {
    bool valid = false;
    uint32_t mapSize = 0;
    uint32_t cascadeCount = 0;
    // Concentric world->light-clip cascades, ordered near→far. `cascades.back()` is the full far
    // radius (== the legacy single map). Resized to the active cascade count on refit by
    // refreshShadowFrustum; receivers read all of them, the caster picks one per pass. These BASE
    // cascades are fitted + texel-snapped at `cachedZoom` (what the caster renders the depth map with
    // on a refit). Don't sample these directly during a zoom — use `liveCascades` (rescaled to the
    // live zoom).
    std::vector<mat4> cascades;

    // `cascades` rescaled to the LIVE zoom each frame (× 1/2^(zoom-cachedZoom)). World coords scale
    // with zoom, so between refits the cached depth map (rendered at cachedZoom) only samples aligned
    // through these rescaled matrices — that is what lets a zoom reuse the cached map with no
    // re-render and no drift/flicker. On a refit frame liveCascades == cascades (ratio 1). Tweakers
    // sample THIS.
    std::vector<mat4> liveCascades;

    // Sticky shadow-map cache: buildings + light are static, so the shadow map for a fixed world
    // region is frame-invariant. Cache the fitted frustum + the world footprint it covers and re-fit
    // (→ re-render the caster pass) ONLY when the camera's required coverage leaves the cached
    // footprint (pan past the oversized margin), the zoom changes enough that a cached matrix would
    // misalign, the active cascade count changes, the map size changes, or a caster set changes.
    // Between refits the cached shadow map is REUSED — so pan / rotate / pitch (all constant-scale)
    // keep shadows visible + stable at ~zero per-frame cost, with no pop. See refreshShadowFrustum().
    vec3 cachedCenter{};          // world-space center the cached frustum is fitted around
    double cachedFarRadius = 0.0; // OVERSIZED far radius (gives pan headroom beyond the live view)
    double cachedZoom = -1.0;     // zoom the cache was fitted at (refit when it drifts too far)
    bool castersDirty = false;    // a fill-extrusion layer changed its caster set → force a refit

    // Latch: has a caster pass actually rendered ≥1 caster into the shadow map? Set by
    // ShadowDepthTweaker once it draws a caster; never reset (the map persists and refills on later
    // refits). The RECEIVER + GROUND tweakers gate shadows on this and stay lit while it is false.
    // Rationale: the caster pass renders the shadow map AFTER the receiver tweakers set up their
    // uniforms on the CPU, but BEFORE the receiver samples on the GPU. Until a caster pass has ever
    // populated the map, its eagerly-created texture reads all-nearest, so a receiver that sampled it
    // would read every roof as shadowed → a uniform grey wash (the D3 device bug). Deferring shadow
    // application until this latches true costs at most the first frame(s) of shadowing and makes the
    // receiver immune to sampling an un-rendered map. Once latched (true for every settled frame) the
    // receiver path is byte-identical to pre-fix.
    bool shadowMapUsable = false;
};

using ShadowFrustumStatePtr = std::shared_ptr<ShadowFrustumState>;

/// Per-frame world->light-clip matrix, fitted from camera state only. The matrix is cached in
/// ShadowFrustumState and shared by caster + receivers so all shadow-map writes and reads use the
/// same light frustum for a rendered frame.
///
/// Pure, unit-testable core: given the camera TransformState and a world-space sun direction,
/// produce the world->light-clip matrix. A correct world-anchored light depends ONLY on the
/// look-at point, zoom and the sun — NOT on camera pitch — so a Transform fixture in a gtest could
/// assert pitch-invariance directly. No such test exists yet (a real frustum fixture is more
/// involved than this wave's scope) — potential follow-up.
mat4 computeWorldToLightClip(const TransformState& state, const vec3& sunDir, uint32_t mapSize);

/// Convenience overload: derives the camera state and the (anchor-aware) sun direction from the
/// frame's paint parameters, then delegates to the testable core above.
mat4 computeWorldToLightClip(const PaintParameters&, uint32_t mapSize);

/// Cascaded variant: returns `cascadeCount` CONCENTRIC world->light-clip matrices that share the
/// same focal center and sun-ground axes and differ only in radius (index cascadeCount-1 = the full
/// far radius; nearer indices shrink by `split` per step, floored at the screen-extent minimum).
/// All cascades are bearing-invariant. cascadeCount==1 returns exactly the single-map matrix.
/// When `overrideCenter` is non-null, the frustum is fitted around that world center with
/// `overrideFarRadius` instead of the live camera's view footprint — used by the sticky cache to
/// build an OVERSIZED, position-pinned frustum that survives panning. Default args = legacy behavior.
std::vector<mat4> computeWorldToLightClipCascades(const TransformState& state,
                                                  const vec3& sunDir,
                                                  uint32_t mapSize,
                                                  uint32_t cascadeCount,
                                                  float split,
                                                  const vec3* overrideCenter = nullptr,
                                                  double overrideFarRadius = 0.0);

/// PaintParameters overload of the cascaded variant (derives state + anchor-aware sun direction).
std::vector<mat4> computeWorldToLightClipCascades(const PaintParameters&, uint32_t mapSize,
                                                  uint32_t cascadeCount, float split);

/// Sticky-cache entry point. Re-fits `fs` (and reports `true`) only when the cached frustum no longer
/// covers the live camera's required footprint, the zoom drifts past the refit policy, the active
/// cascade count or map size changes, or `fs.castersDirty` is set; otherwise leaves the base cascades
/// untouched and returns `false` (the caller reuses the existing shadow map). Either way it refreshes
/// `fs.liveCascades` (the base cascades rescaled to the live zoom). The orchestrator calls this once
/// per frame BEFORE deciding whether to render the caster pass; the tweakers call it again (normally a
/// cache hit) to read `fs.liveCascades`. `activeCascades` is the pitch-gated count for this frame.
bool refreshShadowFrustum(ShadowFrustumState& fs,
                          const TransformState& state,
                          const vec3& sunDir,
                          uint32_t mapSize,
                          uint32_t activeCascades,
                          float split);

/// Tweaker for the shadow-caster layer group: writes each caster drawable's
/// ShadowDepthDrawableUBO.light_matrix = worldToLightClip * matrixFor(tile).
class ShadowDepthTweaker : public LayerTweaker {
public:
    ShadowDepthTweaker(std::string id_,
                       Immutable<style::LayerProperties> props,
                       uint32_t mapSize_,
                       ShadowFrustumStatePtr frustumState_,
                       uint32_t cascadeIndex_ = 0)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_),
          frustumState(std::move(frustumState_)),
          cascadeIndex(cascadeIndex_) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
    ShadowFrustumStatePtr frustumState;
    // Which concentric cascade this caster pass renders into — selects frustumState->cascades[idx].
    uint32_t cascadeIndex;
};

/// Tweaker for the shadow-receiving fill-extrusion drawables: populates
/// FillExtrusionShadowDrawableUBO + FillExtrusionShadowPropsUBO (lighting + per-tile light
/// matrix + shadow intensity/texel/bias).
class FillExtrusionShadowTweaker : public LayerTweaker {
public:
    FillExtrusionShadowTweaker(std::string id_,
                               Immutable<style::LayerProperties> props,
                               uint32_t mapSize_,
                               ShadowFrustumStatePtr frustumState_)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_),
          frustumState(std::move(frustumState_)) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
    ShadowFrustumStatePtr frustumState;
};

/// Tweaker for the z=0 ground-shadow receiver quads. Uses the same tile matrix and
/// light-space matrix as the fill-extrusion shadow receiver.
class GroundShadowTweaker : public LayerTweaker {
public:
    GroundShadowTweaker(std::string id_,
                        Immutable<style::LayerProperties> props,
                        uint32_t mapSize_,
                        ShadowFrustumStatePtr frustumState_)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_),
          frustumState(std::move(frustumState_)) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
    ShadowFrustumStatePtr frustumState;
};

} // namespace mbgl
