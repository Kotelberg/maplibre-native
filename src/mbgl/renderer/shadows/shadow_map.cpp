#include <mbgl/renderer/shadows/shadow_map.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/layer_group.hpp>

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
}

TileLayerGroup* ShadowMap::casterGroup() const {
    return renderTarget ? static_cast<TileLayerGroup*>(renderTarget->getLayerGroup(0).get()) : nullptr;
}

const gfx::Texture2DPtr& ShadowMap::texture() const {
    return renderTarget->getTexture();
}

} // namespace mbgl
