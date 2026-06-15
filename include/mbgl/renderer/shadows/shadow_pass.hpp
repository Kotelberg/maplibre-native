#pragma once

#include <mbgl/renderer/shadows/shadow_map.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

/// Compile-time upper bound on cascades (sizes the per-receiver UBO matrix arrays + shader
/// varyings). The runtime count (`shadowCascadeCount`) is clamped to [1, this].
inline constexpr uint32_t kMaxShadowCascades = 4;

/// Number of concentric, bearing-invariant shadow cascades. `MLN_SHADOW_CASCADE_COUNT` override;
/// default 2 (near = crisp building/near shadows, far = pitched-horizon coverage). Clamped to
/// [1, kMaxShadowCascades]; =1 reproduces the legacy single-map path exactly.
uint32_t shadowCascadeCount();

/// Concentric split factor: cascade 0's radius = max(minRadius, split * farRadius); intermediate
/// cascades interpolate geometrically up to the full far radius. `MLN_SHADOW_CASCADE_SPLIT`
/// override; default 0.4. Clamped to (0, 1).
float shadowCascadeSplit();

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
    ShadowPass(uint32_t mapSize, uint32_t cascadeCount);
    ~ShadowPass();

    /// Idempotent: create the per-cascade depth-capable shadow maps.
    void ensure(gfx::Context&);

    /// Number of concentric cascades this pass owns (1..kMaxShadowCascades), near→far.
    uint32_t cascadeCount() const { return cascadeCount_; }

    /// The shadow-map RenderTarget for cascade `cascadeIdx` — each registered once with the
    /// orchestrator (AddRenderTargetRequest). Ordered near→far, matching ShadowFrustumState::cascades.
    RenderTargetPtr target(uint32_t cascadeIdx) const;

    /// The shadow texture for cascade `cascadeIdx` that receivers sample.
    const gfx::Texture2DPtr& texture(uint32_t cascadeIdx) const;

    /// The per-frame world->light-clip matrix cache, shared by caster + every receiver so they
    /// register against an identical frustum.
    const ShadowFrustumStatePtr& frustumState() const { return frustumState_; }

    /// Per-`{layerID, tileID}` caster registry: returns (lazily creating) the caster TileLayerGroup
    /// for `layerID`, added to the shared shadow RenderTarget at a distinct index. All caster groups
    /// render into the one shadow texture (depth-tested, nearest-to-light wins across layers); each
    /// layer prunes only its own group's tiles, so one layer's tile removal never evicts another's
    /// casters.
    TileLayerGroup* casterGroupFor(gfx::Context&, const std::string& layerID, uint32_t cascadeIdx);

    /// Drop a layer's caster group (layer removed). Removes it from the shared RenderTarget.
    void releaseCasterGroup(const std::string& layerID);

    /// Drop all caster drawables (teardown when shadows go inactive — cast-shadows:false or the env
    /// kill-switch — so no stale casters keep rendering into the shared shadow map). The groups + the
    /// map are retained; they refill on the next active frame via the layers' missing-sidecar rebuild.
    void clearCasters();

    uint32_t mapSize() const { return mapSize_; }
    bool ready() const { return !shadowMaps_.empty() && shadowMaps_.front() != nullptr; }

private:
    uint32_t mapSize_;
    uint32_t cascadeCount_;
    // One depth-capable shadow map per cascade (near→far). Allocated lazily by ensure().
    std::vector<std::unique_ptr<ShadowMap>> shadowMaps_;
    ShadowFrustumStatePtr frustumState_ = std::make_shared<ShadowFrustumState>();
    // Caster registry keyed by {layer id, cascade index}: each entry is that layer's caster group on
    // that cascade's RenderTarget (the first layer on a cascade reuses the map's built-in group 0).
    std::map<std::pair<std::string, uint32_t>, LayerGroupBasePtr> casterGroups_;
    // Next free RenderTarget layer index per cascade (index 0 is the built-in caster group).
    std::vector<int32_t> nextCasterIndex_;
};

} // namespace mbgl
