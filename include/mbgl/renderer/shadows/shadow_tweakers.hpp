#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/layers/fill_extrusion_layer_tweaker.hpp> // BuildingGrowState
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
    // refreshShadowFrustum; receivers read all of them, the caster picks one per pass.
    std::vector<mat4> cascades;

    // --- Sticky cache (Tier 1): buildings + light are static, so the shadow map for a fixed world
    // region is frame-invariant. Cache the fitted frustum + the world footprint it covers and re-fit
    // (→ re-render the caster pass) ONLY when the camera's required coverage leaves the cached
    // footprint (pan past the oversized margin), the zoom changes (world coords scale with zoom, so a
    // cached matrix would misalign), the active cascade count changes, the map size changes, or a
    // caster set changes. Between refits the cached shadow map is REUSED — so pan / rotate / pitch
    // (all constant-scale) keep shadows visible + stable at ~zero per-frame cost, with no pop. See
    // refreshShadowFrustum().
    vec3 cachedCenter{};          // world-space center the cached frustum is fitted around
    double cachedFarRadius = 0.0; // OVERSIZED far radius (gives pan headroom beyond the live view)
    double cachedZoom = -1.0;     // zoom the cache was fitted at (refit when it drifts past a band)
    bool castersDirty = false;    // a fill-extrusion layer changed its caster set → force a refit
};

using ShadowFrustumStatePtr = std::shared_ptr<ShadowFrustumState>;

/// Per-frame world->light-clip matrix, fitted from camera state only. The matrix is cached in
/// ShadowFrustumState and shared by caster + receivers so all shadow-map writes and reads use the
/// same light frustum for a rendered frame.
///
/// Pure, unit-testable core: given the camera TransformState and a world-space sun direction,
/// produce the world->light-clip matrix. A correct world-anchored light depends ONLY on the
/// look-at point, zoom and the sun — NOT on camera pitch — so a Transform fixture in a gtest can
/// assert pitch-invariance directly (see test/renderer/shadow_frustum.test.cpp).
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
std::vector<mat4> computeWorldToLightClipCascades(const PaintParameters&,
                                                  uint32_t mapSize,
                                                  uint32_t cascadeCount,
                                                  float split);

/// Sticky-cache entry point (Tier 1). Re-fits `fs` (and reports `true`) only when the cached frustum
/// no longer covers the live camera's required footprint, the zoom drifts past a band, the active
/// cascade count or map size changes, or `fs.castersDirty` is set; otherwise leaves `fs` untouched
/// and returns `false` (the caller reuses the existing shadow map). The orchestrator calls this once
/// per frame BEFORE deciding whether to render the caster pass; the tweakers call it again (a cache
/// hit) to read `fs.cascades`. `activeCascades` is the pitch-gated count for this frame.
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
    // Per-tile grow-in bookkeeping for the shadow-receiver building path (shared logic with the
    // shadows-off FillExtrusionLayerTweaker so both rise identically).
    BuildingGrowState growState;
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
