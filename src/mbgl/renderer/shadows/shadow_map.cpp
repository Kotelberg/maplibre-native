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
    // NOTE: withDepth=false for now — enabling the depth attachment silently breaks the color
    // write in the RenderTarget pipeline (mbgl plumbing gap), and color-only is needed so the
    // caster's packed depth lands in the map. Consequence: no hardware depth test, so the caster
    // is last-write-wins rather than nearest — inter-building occlusion is unreliable until the
    // depth-attachment path is fixed (or a single-channel min-blend approach is used). See notes.
    renderTarget = std::make_shared<RenderTarget>(
        context, Size{mapSize, mapSize}, gfx::TextureChannelDataType::UnsignedByte, /*withDepth=*/false);
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
