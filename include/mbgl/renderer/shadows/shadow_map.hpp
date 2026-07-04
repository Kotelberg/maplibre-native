#pragma once

#include <mbgl/util/size.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace mbgl {

namespace gfx {
class Context;
class Texture2D;
using Texture2DPtr = std::shared_ptr<Texture2D>;
} // namespace gfx

class RenderTarget;
class TileLayerGroup;

/// Owns the offscreen shadow-depth RenderTarget (RGBA8 packed linear depth + a hardware
/// depth attachment so the nearest caster wins) and the caster layer group that renders
/// fill-extrusion geometry from the sun's POV. Backend-agnostic; only instantiated under
/// the Metal gate in S1.
class ShadowMap {
public:
    explicit ShadowMap(uint32_t mapSize);

    /// Create the depth-capable target + an empty caster TileLayerGroup (idempotent).
    void ensure(gfx::Context&, const std::string& layerID);

    const std::shared_ptr<RenderTarget>& target() const { return renderTarget; }
    TileLayerGroup* casterGroup() const;
    const gfx::Texture2DPtr& texture() const;
    uint32_t size() const { return mapSize; }

private:
    uint32_t mapSize;
    std::shared_ptr<RenderTarget> renderTarget;
};

} // namespace mbgl
