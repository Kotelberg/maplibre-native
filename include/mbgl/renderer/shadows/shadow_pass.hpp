#pragma once

#include <mbgl/renderer/shadows/shadow_map.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

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
using RenderTargetPtr = std::shared_ptr<RenderTarget>;
class TileLayerGroup;
class LayerGroupBase;
using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

/// Renderer-owned directional-shadow pass (the light-owned architecture; see SHADOW_REWRITE_DESIGN.md).
///
/// Owns ONE shared shadow system for the whole frame — the depth-capable shadow map + the caster
/// layer group + the per-frame light frustum + the single ground-shadow receiver group — replacing
/// the previous per-RenderFillExtrusionLayer ownership. ALL shadow-casting 3D layers register their
/// caster drawables into the shared caster group (keyed by {layerID, tileID}); ALL receiver layers
/// sample the one shared texture with the one shared worldToLightClip. This eliminates the multi-
/// layer cost (N shadow maps / N caster passes) and the double-cast-shadow hazard of the per-layer
/// design on styles with several fill-extrusion layers (e.g. HataHub's building-3d + hover + listing
/// + selected).
///
/// Owned by RenderOrchestrator; only instantiated under the Metal gate.
class ShadowPass {
public:
    explicit ShadowPass(uint32_t mapSize);
    ~ShadowPass();

    /// Idempotent: create the depth-capable shadow map (with its caster group) + the per-frame
    /// frustum state + the single ground-shadow layer group at the given layer index.
    void ensure(gfx::Context&, int32_t groundLayerIndex);

    /// The shadow-map RenderTarget — registered once with the orchestrator (AddRenderTargetRequest).
    RenderTargetPtr target() const;

    /// All shadow-casting layers add their caster (depth-only) drawables here, tagged by layer id.
    TileLayerGroup* casterGroup() const;

    /// The single ground-shadow receiver layer group (z=0 quads), drawn once at the insertion point.
    LayerGroupBase* groundGroup() const { return groundGroup_.get(); }
    const LayerGroupBasePtr& groundGroupPtr() const { return groundGroup_; }

    /// The shared shadow texture all receivers sample.
    const gfx::Texture2DPtr& texture() const;

    /// The per-frame world->light-clip matrix cache, shared by caster + every receiver so they
    /// register against an identical frustum.
    const ShadowFrustumStatePtr& frustumState() const { return frustumState_; }

    uint32_t mapSize() const { return mapSize_; }
    bool ready() const { return shadowMap_ != nullptr; }

private:
    uint32_t mapSize_;
    std::unique_ptr<ShadowMap> shadowMap_;
    LayerGroupBasePtr groundGroup_;
    ShadowFrustumStatePtr frustumState_ = std::make_shared<ShadowFrustumState>();
};

} // namespace mbgl
