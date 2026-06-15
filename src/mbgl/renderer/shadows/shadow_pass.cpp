#include <mbgl/renderer/shadows/shadow_pass.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/renderer/render_target.hpp>

#include <cstdlib>
#include <string_view>

namespace mbgl {

bool shadowsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MLN_RENDER_3D_ENHANCEMENTS");
        return !(v && std::string_view(v) == "0");
    }();
    return enabled;
}

uint32_t shadowMapSize() {
    static const uint32_t size = [] {
        const char* v = std::getenv("MLN_SHADOW_MAP_SIZE");
        return v ? static_cast<uint32_t>(std::atoi(v)) : 1024u;
    }();
    return size;
}

ShadowPass::ShadowPass(uint32_t mapSize)
    : mapSize_(mapSize) {}

ShadowPass::~ShadowPass() = default;

void ShadowPass::ensure(gfx::Context& context) {
    if (!shadowMap_) {
        shadowMap_ = std::make_unique<ShadowMap>(mapSize_);
        shadowMap_->ensure(context, "shadow-pass");
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
