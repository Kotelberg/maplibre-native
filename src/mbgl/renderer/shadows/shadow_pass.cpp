#include <mbgl/renderer/shadows/shadow_pass.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/layer_group.hpp>

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
        // ShadowMap::ensure creates one caster group at RenderTarget index 0 — claim it as the
        // first registry slot so the common single-FE-layer case reuses it (no extra group).
        nextCasterIndex_ = 1;
    }
}

RenderTargetPtr ShadowPass::target() const {
    return shadowMap_ ? shadowMap_->target() : nullptr;
}

const gfx::Texture2DPtr& ShadowPass::texture() const {
    return shadowMap_->texture();
}

TileLayerGroup* ShadowPass::casterGroupFor(gfx::Context& context, const std::string& layerID) {
    if (const auto it = casterGroups_.find(layerID); it != casterGroups_.end()) {
        return static_cast<TileLayerGroup*>(it->second.get());
    }
    if (!shadowMap_) {
        return nullptr;
    }

    LayerGroupBasePtr group;
    if (casterGroups_.empty()) {
        // Reuse the shadow map's built-in caster group (index 0) for the first registered layer.
        group = shadowMap_->target()->getLayerGroup(0);
    }
    if (!group) {
        group = context.createTileLayerGroup(
            nextCasterIndex_++, /*initialCapacity=*/64, "shadow-pass-casters-" + layerID);
        shadowMap_->target()->addLayerGroup(group, /*replace=*/false);
    }
    casterGroups_[layerID] = group;
    return static_cast<TileLayerGroup*>(group.get());
}

void ShadowPass::releaseCasterGroup(const std::string& layerID) {
    const auto it = casterGroups_.find(layerID);
    if (it == casterGroups_.end()) {
        return;
    }
    if (shadowMap_ && it->second) {
        shadowMap_->target()->removeLayerGroup(it->second->getLayerIndex());
    }
    casterGroups_.erase(it);
}

void ShadowPass::clearCasters() {
    for (auto& entry : casterGroups_) {
        if (auto* group = static_cast<TileLayerGroup*>(entry.second.get())) {
            group->clearDrawables();
        }
    }
}

} // namespace mbgl
