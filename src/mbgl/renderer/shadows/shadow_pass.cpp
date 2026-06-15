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
        // 2048 default: at 1024 the casters fill only ~10% of the map (the frustum over-covers), so
        // effective texel density on the buildings was coarse → staircased shadow edges. 2048 is the
        // balanced midpoint (1024 staircased, 4096 crisp; 4x cheaper than 4096) and, with the tighter
        // radius floor, restores crisp edges. iOS-Metal target handles it; env-override for low-end.
        return v ? static_cast<uint32_t>(std::atoi(v)) : 2048u;
    }();
    return size;
}

uint32_t shadowCascadeCount() {
    static const uint32_t count = [] {
        const char* v = std::getenv("MLN_SHADOW_CASCADE_COUNT");
        const uint32_t n = v ? static_cast<uint32_t>(std::atoi(v)) : 2u;
        // Clamp to [1, kMaxShadowCascades]: 1 = legacy single map; the UBO/shader arrays are sized
        // to kMaxShadowCascades, so a larger value would overrun them.
        return n < 1u ? 1u : (n > kMaxShadowCascades ? kMaxShadowCascades : n);
    }();
    return count;
}

float shadowCascadeSplit() {
    static const float split = [] {
        const char* v = std::getenv("MLN_SHADOW_CASCADE_SPLIT");
        const float s = v ? static_cast<float>(std::atof(v)) : 0.4f;
        // Keep strictly inside (0,1): 0 collapses the near cascade, 1 makes it equal the far cascade.
        return s <= 0.05f ? 0.05f : (s >= 0.95f ? 0.95f : s);
    }();
    return split;
}

ShadowPass::ShadowPass(uint32_t mapSize, uint32_t cascadeCount)
    : mapSize_(mapSize),
      cascadeCount_(cascadeCount < 1u ? 1u : cascadeCount) {}

ShadowPass::~ShadowPass() = default;

void ShadowPass::ensure(gfx::Context& context) {
    if (!shadowMaps_.empty()) {
        return;
    }
    shadowMaps_.reserve(cascadeCount_);
    // Index 0 of each cascade's RenderTarget is its built-in caster group (claimed by the first
    // layer registered on that cascade); fresh groups start at index 1.
    nextCasterIndex_.assign(cascadeCount_, 1);
    for (uint32_t c = 0; c < cascadeCount_; ++c) {
        auto map = std::make_unique<ShadowMap>(mapSize_);
        map->ensure(context, "shadow-pass-c" + std::to_string(c));
        shadowMaps_.push_back(std::move(map));
    }
}

RenderTargetPtr ShadowPass::target(uint32_t cascadeIdx) const {
    return cascadeIdx < shadowMaps_.size() && shadowMaps_[cascadeIdx] ? shadowMaps_[cascadeIdx]->target()
                                                                      : nullptr;
}

const gfx::Texture2DPtr& ShadowPass::texture(uint32_t cascadeIdx) const {
    return shadowMaps_[cascadeIdx]->texture();
}

TileLayerGroup* ShadowPass::casterGroupFor(gfx::Context& context, const std::string& layerID, uint32_t cascadeIdx) {
    if (cascadeIdx >= shadowMaps_.size() || !shadowMaps_[cascadeIdx]) {
        return nullptr;
    }
    const auto key = std::make_pair(layerID, cascadeIdx);
    if (const auto it = casterGroups_.find(key); it != casterGroups_.end()) {
        return static_cast<TileLayerGroup*>(it->second.get());
    }
    auto& map = shadowMaps_[cascadeIdx];

    // Reuse THIS cascade's built-in caster group (RenderTarget index 0) for the first layer
    // registered on it; later layers get a fresh group at the next index on the same target.
    bool cascadeHasLayer = false;
    for (const auto& e : casterGroups_) {
        if (e.first.second == cascadeIdx) {
            cascadeHasLayer = true;
            break;
        }
    }
    LayerGroupBasePtr group;
    if (!cascadeHasLayer) {
        group = map->target()->getLayerGroup(0);
    }
    if (!group) {
        group = context.createTileLayerGroup(nextCasterIndex_[cascadeIdx]++,
                                             /*initialCapacity=*/64,
                                             "shadow-pass-casters-" + layerID + "-c" + std::to_string(cascadeIdx));
        map->target()->addLayerGroup(group, /*replace=*/false);
    }
    casterGroups_[key] = group;
    return static_cast<TileLayerGroup*>(group.get());
}

void ShadowPass::releaseCasterGroup(const std::string& layerID) {
    // Remove this layer's caster group from EVERY cascade it was registered on.
    for (auto it = casterGroups_.begin(); it != casterGroups_.end();) {
        if (it->first.first == layerID) {
            const uint32_t cascadeIdx = it->first.second;
            if (cascadeIdx < shadowMaps_.size() && shadowMaps_[cascadeIdx] && it->second) {
                shadowMaps_[cascadeIdx]->target()->removeLayerGroup(it->second->getLayerIndex());
            }
            it = casterGroups_.erase(it);
        } else {
            ++it;
        }
    }
}

void ShadowPass::clearCasters() {
    for (auto& entry : casterGroups_) {
        if (auto* group = static_cast<TileLayerGroup*>(entry.second.get())) {
            group->clearDrawables();
        }
    }
}

} // namespace mbgl
