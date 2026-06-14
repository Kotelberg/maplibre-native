#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/util/mat4.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace mbgl {

class PaintParameters;
class LayerGroupBase;

struct ShadowFrustumState {
    bool valid = false;
    uint64_t frameCount = 0;
    uint32_t mapSize = 0;
    mat4 worldToLightClip;
};

using ShadowFrustumStatePtr = std::shared_ptr<ShadowFrustumState>;

/// Per-frame world->light-clip matrix, fitted from camera state only. The matrix is cached in
/// ShadowFrustumState and shared by caster + receivers so all shadow-map writes and reads use the
/// same light frustum for a rendered frame.
mat4 computeWorldToLightClip(const PaintParameters&, uint32_t mapSize);

/// Tweaker for the shadow-caster layer group: writes each caster drawable's
/// ShadowDepthDrawableUBO.light_matrix = worldToLightClip * matrixFor(tile).
class ShadowDepthTweaker : public LayerTweaker {
public:
    ShadowDepthTweaker(std::string id_,
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
