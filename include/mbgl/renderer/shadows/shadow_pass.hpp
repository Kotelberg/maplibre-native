#pragma once

#include <mbgl/renderer/shadows/shadow_map.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

#include <cstdint>
#include <map>
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

/// Master runtime gate for the directional-shadow path (Metal-only). Default ON;
/// `MLN_RENDER_3D_ENHANCEMENTS=0` force-disables it for the byte-identical-off check and
/// per-device benchmark gating. Single source of truth shared by the orchestrator (which owns the
/// pass) and the render layers (which register casters/receivers).
bool shadowsEnabled();

/// Shadow-map resolution (square). `MLN_SHADOW_MAP_SIZE` override; default 1024.
uint32_t shadowMapSize();

/// Renderer-owned directional-shadow pass (the light-owned architecture; see
/// SHADOW_REWRITE_DESIGN.md).
///
/// Owns ONE shared shadow system for the whole frame — the depth-capable shadow map (RenderTarget +
/// hardware depth + RGBA8 packed-depth texture) and the per-frame light frustum — replacing the
/// previous per-RenderFillExtrusionLayer ownership. ALL shadow-casting 3D layers register their
/// caster drawables into a per-layer caster group (keyed by layer id; §3.3 registry); ALL receiver
/// layers sample the one shared texture with the one shared `worldToLightClip` (cached per-frame in
/// `frustumState`). This eliminates the multi-layer cost (N shadow maps / N caster passes) and the
/// cross-layer caster eviction + multi-ground-draw hazards of the per-layer design on styles with
/// several fill-extrusion layers (e.g. HataHub's staged style: building-3d + hover + listings +
/// selected).
///
/// Owned by RenderOrchestrator; only instantiated under the Metal gate. The orchestrator owns the
/// RenderTarget lifecycle (registers it once via AddRenderTargetRequest); layers must NOT add or
/// remove the shared target on their own lifecycle events.
class ShadowPass {
public:
    explicit ShadowPass(uint32_t mapSize);
    ~ShadowPass();

    /// Idempotent: create the depth-capable shadow map.
    void ensure(gfx::Context&);

    /// The shadow-map RenderTarget — registered once with the orchestrator (AddRenderTargetRequest).
    RenderTargetPtr target() const;

    /// The shared shadow texture all receivers sample.
    const gfx::Texture2DPtr& texture() const;

    /// The per-frame world->light-clip matrix cache, shared by caster + every receiver so they
    /// register against an identical frustum.
    const ShadowFrustumStatePtr& frustumState() const { return frustumState_; }

    /// Per-`{layerID, tileID}` caster registry: returns (lazily creating) the caster TileLayerGroup
    /// for `layerID`, added to the shared shadow RenderTarget at a distinct index. All caster groups
    /// render into the one shadow texture (depth-tested, nearest-to-light wins across layers); each
    /// layer prunes only its own group's tiles, so one layer's tile removal never evicts another's
    /// casters.
    TileLayerGroup* casterGroupFor(gfx::Context&, const std::string& layerID);

    /// Drop a layer's caster group (layer removed). Removes it from the shared RenderTarget.
    void releaseCasterGroup(const std::string& layerID);

    uint32_t mapSize() const { return mapSize_; }
    bool ready() const { return shadowMap_ != nullptr; }

private:
    uint32_t mapSize_;
    std::unique_ptr<ShadowMap> shadowMap_;
    ShadowFrustumStatePtr frustumState_ = std::make_shared<ShadowFrustumState>();
    // Caster registry: layer id -> its caster group (each at a distinct RenderTarget layer index).
    std::map<std::string, LayerGroupBasePtr> casterGroups_;
    int32_t nextCasterIndex_ = 0;
};

} // namespace mbgl
