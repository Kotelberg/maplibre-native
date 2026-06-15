#include <mbgl/renderer/shadows/shadow_pass.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/layer_group.hpp>

namespace mbgl {

ShadowPass::ShadowPass(uint32_t mapSize)
    : mapSize_(mapSize) {}

ShadowPass::~ShadowPass() = default;

void ShadowPass::ensure(gfx::Context& context, int32_t groundLayerIndex) {
    if (!shadowMap_) {
        shadowMap_ = std::make_unique<ShadowMap>(mapSize_);
        shadowMap_->ensure(context, "shadow-pass");
    }
    if (!groundGroup_) {
        // One ground-shadow receiver group (z=0 quads) shared by the whole frame, inserted at the
        // style-controlled index (shadow-draw-before-layer; defaults below the building layers).
        groundGroup_ = context.createTileLayerGroup(groundLayerIndex, /*initialCapacity=*/64, "shadow-pass-ground");
    }
}

RenderTargetPtr ShadowPass::target() const {
    return shadowMap_ ? shadowMap_->target() : nullptr;
}

TileLayerGroup* ShadowPass::casterGroup() const {
    return shadowMap_ ? shadowMap_->casterGroup() : nullptr;
}

const gfx::Texture2DPtr& ShadowPass::texture() const {
    return shadowMap_->texture();
}

} // namespace mbgl
