#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/util/mat4.hpp>

#include <cstdint>

namespace mbgl {

class PaintParameters;
class LayerGroupBase;

/// Per-frame world->light-clip matrix, fitted to the world rects of the tiles currently in
/// `layerGroup` (height-expanded), using the sun direction from the evaluated light. Both the
/// caster and receiver tweakers call this so they derive the identical matrix.
mat4 computeWorldToLightClip(LayerGroupBase& layerGroup, const PaintParameters&, uint32_t mapSize);

/// Tweaker for the shadow-caster layer group: writes each caster drawable's
/// ShadowDepthDrawableUBO.light_matrix = worldToLightClip * matrixFor(tile).
class ShadowDepthTweaker : public LayerTweaker {
public:
    ShadowDepthTweaker(std::string id_, Immutable<style::LayerProperties> props, uint32_t mapSize_)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
};

/// Tweaker for the shadow-receiving fill-extrusion drawables: populates
/// FillExtrusionShadowDrawableUBO + FillExtrusionShadowPropsUBO (lighting + per-tile light
/// matrix + shadow intensity/texel/bias).
class FillExtrusionShadowTweaker : public LayerTweaker {
public:
    FillExtrusionShadowTweaker(std::string id_, Immutable<style::LayerProperties> props, uint32_t mapSize_)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
};

/// Tweaker for the z=0 ground-shadow receiver quads. Uses the same tile matrix and
/// light-space matrix as the fill-extrusion shadow receiver.
class GroundShadowTweaker : public LayerTweaker {
public:
    GroundShadowTweaker(std::string id_, Immutable<style::LayerProperties> props, uint32_t mapSize_)
        : LayerTweaker(std::move(id_), std::move(props)),
          mapSize(mapSize_) {}
    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    uint32_t mapSize;
};

} // namespace mbgl
