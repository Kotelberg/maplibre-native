#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/chrono.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

namespace mbgl {

// Building "grow-in" reveal: when a fill-extrusion tile first renders, its extrusion height ramps
// 0→full over this duration so buildings rise from the ground as tiles load. This hides the clumpy
// per-tile pop-in during a zoom-in burst (z10→z15) without changing the (parse-bound) total load
// time — it turns the freeze into a smooth, organic fill. 0 disables (full height immediately).
// Single source of truth for both the per-tile factor (tweaker) and the repaint window
// (RenderOrchestrator). Env override MLN_BUILDING_GROW_MS. Inline so the env is read once process-wide.
inline std::chrono::milliseconds buildingGrowDurationMs() {
    static const std::chrono::milliseconds ms = [] {
        const char* v = std::getenv("MLN_BUILDING_GROW_MS");
        // Default OFF: the load-time grow-in is cosmetic and the team chose platform consistency
        // (both Android GL + iOS Metal show only the style's zoom-driven height ramp, no load rise).
        // Opt in per-process with MLN_BUILDING_GROW_MS=<ms> for experiments.
        const int n = v ? std::atoi(v) : 0;
        return std::chrono::milliseconds(n < 0 ? 0 : n);
    }();
    return ms;
}

// Per-tile grow-in bookkeeping shared by the plain (FillExtrusionLayerTweaker) and shadow-receiver
// (FillExtrusionShadowTweaker) building paths so both rise identically. A building rises ONCE, when it
// first appears in an area — never again while it stays visible, even across zoom changes that reload
// tiles. One instance per tweaker; not thread-safe (tweakers run on the render thread).
//
// Keyed by CANONICAL tile id (z/x/y of the data tile), NOT OverscaledTileID: above the source's max
// zoom the same data tile is overzoomed across many display zooms, so canonical is stable while
// zooming in/out and the rise doesn't retrigger. Crossing the data max zoom (e.g. z13 parent ↔ z14
// children) changes the canonical tile, so we additionally treat a tile as already-grown when a
// parent or child canonical is already tracked (the area was already covered). A grace window keeps
// entries briefly after they leave the screen so transient gaps during a zoom/pan don't reset them.
class BuildingGrowState {
public:
    // Call once at the top of execute(). `enabled` folds in both the duration (>0) and Continuous map
    // mode (Static/Tile snapshots must render full height, not a mid-rise frame). `now` is the frame time.
    void beginFrame(bool enabled, TimePoint now) {
        enabled_ = enabled;
        now_ = now;
        if (!enabled_) {
            seen_.clear();
        }
    }
    // Per drawable: returns the tile's grow factor (1.0 when disabled or fully risen). On a tile's
    // first sighting, it animates from 0 — unless a parent/child canonical is already tracked, in
    // which case the area was already showing buildings (a zoom across the data max zoom) and it snaps
    // to full with no animation.
    float factor(const OverscaledTileID& id) {
        if (!enabled_) {
            return 1.0f;
        }
        const CanonicalTileID key = id.canonical;
        auto it = seen_.find(key);
        if (it == seen_.end()) {
            const TimePoint first = coveredByRelative(key) ? (now_ - 2 * buildingGrowDurationMs()) : now_;
            it = seen_.emplace(key, Entry{first, now_}).first;
        }
        it->second.lastSeen = now_;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_ - it->second.firstSeen).count();
        const float t = std::clamp(
            static_cast<float>(ms) / static_cast<float>(buildingGrowDurationMs().count()), 0.0f, 1.0f);
        const float g = 1.0f - std::pow(1.0f - t, 3.0f); // ease-out cubic: quick rise, gentle settle
        // Floor at a small non-zero height: the `building-3d-base-shadow` layer is a thin (≤1.5 m) grey
        // skirt over the same footprint, so at exactly grow==0 both are flat at z=0 and the grey slab
        // tints the building. Any positive height lifts the building above the skirt — start there so
        // there's no <100 ms grey flash on first appearance. Imperceptible as a pop (a few % of height).
        return std::max(g, 0.05f);
    }
    // Call once after visiting all drawables; drops tiles not seen within the grace window so the map
    // stays bounded while surviving brief disappearances during zoom/pan transitions.
    void endFrame() {
        if (!enabled_) {
            return;
        }
        const auto grace = std::chrono::seconds(10);
        for (auto it = seen_.begin(); it != seen_.end();) {
            it = (now_ - it->second.lastSeen > grace) ? seen_.erase(it) : std::next(it);
        }
    }

private:
    struct Entry {
        TimePoint firstSeen; // when the rise started (or backdated to "already full" if covered)
        TimePoint lastSeen;  // last frame this canonical tile was drawn (for grace pruning)
    };
    // True if this tile's immediate parent or any of its 4 children is already tracked — i.e. the
    // ground it covers was already showing buildings, so it must not re-animate.
    bool coveredByRelative(const CanonicalTileID& k) const {
        if (k.z > 0 && seen_.count(CanonicalTileID(static_cast<uint8_t>(k.z - 1), k.x / 2, k.y / 2))) {
            return true;
        }
        const auto cz = static_cast<uint8_t>(k.z + 1);
        for (uint32_t dx = 0; dx < 2; ++dx) {
            for (uint32_t dy = 0; dy < 2; ++dy) {
                if (seen_.count(CanonicalTileID(cz, k.x * 2 + dx, k.y * 2 + dy))) {
                    return true;
                }
            }
        }
        return false;
    }

    bool enabled_ = false;
    TimePoint now_{};
    std::map<CanonicalTileID, Entry> seen_;
};

/**
    Fill extrusion layer specific tweaker
 */
class FillExtrusionLayerTweaker : public LayerTweaker {
public:
    FillExtrusionLayerTweaker(std::string id_, Immutable<style::LayerProperties> properties)
        : LayerTweaker(std::move(id_), properties) {}

public:
    ~FillExtrusionLayerTweaker() override = default;

    void execute(LayerGroupBase&, const PaintParameters&) override;

protected:
    gfx::UniformBufferPtr evaluatedPropsUniformBuffer;

    // Per-tile grow-in bookkeeping for the plain (shadows-off) building path.
    BuildingGrowState growState;

#if MLN_UBO_CONSOLIDATION
    gfx::UniformBufferPtr drawableUniformBuffer;
    gfx::UniformBufferPtr tilePropsUniformBuffer;
#endif
};

} // namespace mbgl
