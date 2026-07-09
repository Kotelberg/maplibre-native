#include <mbgl/renderer/shadows/shadow_map.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/layer_group.hpp>

#include <vector>

namespace mbgl {

ShadowMap::ShadowMap(uint32_t mapSize_)
    : mapSize(mapSize_) {}

void ShadowMap::ensure(gfx::Context& context, const std::string& layerID) {
    if (renderTarget) {
        return;
    }
    // withDepth=true: the caster group runs as is3D drawables (see render_fill_extrusion_layer),
    // so mtl::TileLayerGroup::render applies the group-level depthModeFor3D() (LessEqual + write)
    // against this target's Float32 depth attachment. That makes the NEAREST-to-light caster's
    // packed-depth color survive (hardware depth test) instead of last-write-wins — which is what
    // makes inter-building occlusion reliable. RenderTarget clears color to white (packed far) and
    // depth to 1.0. The earlier "depth breaks color" was the pre-[0,1] z-remap era: with depth
    // testing live, Metal clips NDC z outside [0,1]; ShadowFrustum::fit now remaps to [0,1].
    renderTarget = std::make_shared<RenderTarget>(
        context, Size{mapSize, mapSize}, gfx::TextureChannelDataType::UnsignedByte, /*withDepth=*/true);
    renderTarget->addLayerGroup(context.createTileLayerGroup(0, /*initialCapacity=*/64, layerID + "-shadow-casters"),
                                /*replace=*/true);

    // Eagerly materialise the sampled (color/packed-depth) texture now, at allocation, instead of
    // letting it be created lazily on first render (the mtl OffscreenTextureResource only calls
    // colorTexture->create() inside bind(), i.e. when the target actually renders). Receivers bind
    // ALL allocated cascades' shadow textures up-front, fixed at drawable-build time (see the
    // idFillExtrusionShadowTexture0 loop in render_fill_extrusion_layer.cpp), but a cascade only
    // renders when it is ACTIVE for the frame (activeShadowCascadeCount is pitch-gated). On a
    // low-pitch view the far cascade never renders, so its texture would stay textureDirty and the
    // first 3D receiver to draw trips mtl::Texture2D::bind's assert(!textureDirty) -> SIGABRT.
    // Creating it here keeps every bound cascade texture valid regardless of which cascades render;
    // an unrendered cascade just holds an empty texture the receiver shader never samples (it loops
    // only to the active cascade count). Idempotent on every backend now: mtl::Texture2D::create()
    // always was, and gl::Texture2D::create() is too (it guards allocateTexture on !texture) — so this
    // no longer orphans the GL texture the way it did at 0a81f793 (the GL shadow-blackout bug). This
    // eager materialize is REQUIRED for the multi-cascade (pitch-gated) path on every backend: the
    // far cascade that doesn't render on a flat view still needs a valid texture for the receiver to
    // bind up-front.
    renderTarget->getTexture()->create();

    // Initialise the sampled packed-depth texture to FAR (white == packed depth 1.0) so it reads
    // "nothing casts / everything lit" until a caster pass first renders into it. The RenderTarget
    // clears to white on every render, so a target that renders overwrites this immediately and the
    // result is byte-identical. But a receiver can bind + sample this texture on a frame BEFORE the
    // caster pass has ever populated it (the caster pass is a separate RenderTarget drawn after the
    // receivers set up their uniforms; on some backends/drivers the first caster render can lag the
    // first receiver sample by a frame). An UNINITIALISED texture reads all-zeros == packed depth 0.0
    // == NEAREST, so every roof fragment compares as shadowed → a uniform grey roof wash (the D3
    // device bug). Seeding it to FAR makes that pre-render sample read lit instead of grey.
    const auto& tex = renderTarget->getTexture();
    const std::vector<uint8_t> farPixels(static_cast<size_t>(mapSize) * mapSize * 4u, 0xFF);
    tex->upload(farPixels.data(), Size{mapSize, mapSize});
}

TileLayerGroup* ShadowMap::casterGroup() const {
    return renderTarget ? static_cast<TileLayerGroup*>(renderTarget->getLayerGroup(0).get()) : nullptr;
}

const gfx::Texture2DPtr& ShadowMap::texture() const {
    return renderTarget->getTexture();
}

} // namespace mbgl
