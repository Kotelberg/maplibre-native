#pragma once

#include <mbgl/renderer/shadows/shadow_map.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

#include <cstdint>
#include <memory>

namespace mbgl {

namespace gfx {
class Context;
class Texture2D;
using Texture2DPtr = std::shared_ptr<Texture2D>;
} // namespace gfx

class RenderTarget;
using RenderTargetPtr = std::shared_ptr<RenderTarget>;
class TileLayerGroup;

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
/// hardware depth + RGBA8 packed-depth texture + caster layer group) and the per-frame light
/// frustum — replacing the previous per-RenderFillExtrusionLayer ownership. ALL shadow-casting 3D
/// layers register their caster drawables into the shared caster group; ALL receiver layers sample
/// the one shared texture with the one shared `worldToLightClip` (cached per-frame in
/// `frustumState`). This eliminates the multi-layer cost (N shadow maps / N caster passes) and the
/// double-cast-shadow / multi-ground-draw hazard of the per-layer design on styles with several
/// fill-extrusion layers (e.g. HataHub's staged style: building-3d + hover + listings + selected).
///
/// Owned by RenderOrchestrator; only instantiated under the Metal gate. The orchestrator owns the
/// RenderTarget lifecycle (registers it once via AddRenderTargetRequest); layers must NOT add or
/// remove the shared target on their own lifecycle events.
///
/// P2 (this commit): single shared caster group + shared texture + shared frustum; single-FE-layer
/// parity. P3 adds a per-`{layerID, tileID}` caster registry + a single ground-shadow group.
class ShadowPass {
public:
    explicit ShadowPass(uint32_t mapSize);
    ~ShadowPass();

    /// Idempotent: create the depth-capable shadow map (with its caster group).
    void ensure(gfx::Context&);

    /// The shadow-map RenderTarget — registered once with the orchestrator (AddRenderTargetRequest).
    RenderTargetPtr target() const;

    /// All shadow-casting layers add their caster (depth-only) drawables here.
    TileLayerGroup* casterGroup() const;

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
    ShadowFrustumStatePtr frustumState_ = std::make_shared<ShadowFrustumState>();
};

} // namespace mbgl
