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
