#include <mbgl/renderer/shadows/shadow_pass.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/layer_group.hpp>

#include <cstdlib>
#include <string_view>

namespace mbgl {

uint32_t shadowMapSize() {
    static const uint32_t size = [] {
        const char* v = std::getenv("MLN_SHADOW_MAP_SIZE");
        // 1024 default (was 2048): the shadow caster pass re-rasterizes every visible building into a
        // mapSize×mapSize depth target per cascade EVERY frame, so fill cost is O(mapSize²) — 2048 is 4×
        // the GPU fill of 1024. The texel-density loss that previously staircased 1024 edges is now
        // recovered another way: the pitch-gated cascade scheme (shadowCascadeCount/activeShadowCascade-
        // Count) gives the FLAT top-down view a single auto-sized frustum (radius ≈ the visible screen,
        // ~0.5 world-px/texel at 1024) and the PITCHED view a tight near cascade — so near buildings keep
        // crisp edges without paying 2048 everywhere. Bump back to 2048 via env on a GPU that can spare it.
        return v ? static_cast<uint32_t>(std::atoi(v)) : 1024u;
    }();
    return size;
}

uint32_t shadowCascadeCount() {
    static const uint32_t count = [] {
        // MLN_SHADOW_CASCADE_COUNT (debug knob): how many concentric cascades the pass allocates.
        // Effect: 1 = a single full-radius map (crisper coverage tradeoff); 2 = a tight near cascade
        // plus a far cascade for pitched-horizon coverage. Default 2 on every backend: a single
        // cascade fitted to the whole visible extent is too coarse for the BUILDING receiver at
        // moderate zooms — resolving a neighbour's shadow against the receiver's own depth + bias
        // needs sub-texel precision that the (more forgiving) ground receiver doesn't, so with one
        // cascade building-surface shadows pop in noticeably later than ground shadows while zooming
        // in. The tight near cascade restores receiver-grade texel density and keeps the two receiver
        // onsets in sync. Clamped to [1, kMaxShadowCascades].
        const char* v = std::getenv("MLN_SHADOW_CASCADE_COUNT");
        const uint32_t backendDefault = 2u;
        const uint32_t n = v ? static_cast<uint32_t>(std::atoi(v)) : backendDefault;
        // Clamp to [1, kMaxShadowCascades]: 1 = legacy single map; the UBO/shader arrays are sized
        // to kMaxShadowCascades, so a larger value would overrun them.
        return n < 1u ? 1u : (n > kMaxShadowCascades ? kMaxShadowCascades : n);
    }();
    return count;
}

uint32_t activeShadowCascadeCount(double pitchRadians) {
    const uint32_t allocated = shadowCascadeCount();
    if (allocated <= 1u) {
        return allocated; // nothing to gate
    }
    static const bool gateEnabled = [] {
        const char* v = std::getenv("MLN_SHADOW_PITCH_GATE");
        return !(v && std::string_view(v) == "0");
    }();
    if (!gateEnabled) {
        return allocated;
    }
    // MLN_SHADOW_PITCH_GATE_DEG (debug knob): pitch threshold in DEGREES above which the near cascade
    // activates. Default 20°: a near-top-down view (pitch≈0) drops to the single full-density cascade,
    // while a pitched view engages the near cascade for crisp near buildings. A pitch gesture crosses
    // the threshold once (no flapping) while the camera is already moving, hiding the pop-in.
    static const double thresholdRad = [] {
        const char* v = std::getenv("MLN_SHADOW_PITCH_GATE_DEG");
        const double deg = v ? std::atof(v) : 20.0;
        return deg * 0.017453292519943295; // π/180
    }();
    return pitchRadians >= thresholdRad ? allocated : 1u;
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
