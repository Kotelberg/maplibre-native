#pragma once

#include <algorithm>

namespace mbgl {

// The engine's building grow/fade ramp: a fixed zoom band over which
// extruded buildings (and, by extension, any `model` layer instances placed
// among them) rise from the ground plane to full height. Kept as plain
// constants + free functions (rather than a style paint property) so every
// consumer that needs to stay visually in sync with the building extrusion
// reveal — currently only the model layer's per-frame tweaker — can share
// exactly one ramp definition instead of re-deriving it.
//
// The band is integer-aligned (14→15): a composite (zoom+feature) paint
// expression on the style side quantizes to INTEGER zoom levels, so a
// sub-integer end would desync the C++-side ramp used here from the style's
// own height ramp. Keep both ends on integers if this ever changes.
constexpr float kBuildingExtrusionGrowZoomStart = 14.0f;
constexpr float kBuildingExtrusionGrowZoomEnd = 15.0f;
// The fade covers the first ~60% of the grow window: geometry collapsed
// onto the ground plane (grow≈0) has every face coplanar, which z-fights,
// so it needs its own (shorter) alpha ramp to fade in cleanly.
constexpr float kBuildingExtrusionModelFadeZoomEnd = 14.6f;

inline float buildingExtrusionGrowFactor(double zoom) {
    const double range = kBuildingExtrusionGrowZoomEnd - kBuildingExtrusionGrowZoomStart;
    if (range <= 0.0) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp((zoom - kBuildingExtrusionGrowZoomStart) / range, 0.0, 1.0));
}

inline float buildingExtrusionModelFadeFactor(double zoom) {
    const double range = kBuildingExtrusionModelFadeZoomEnd - kBuildingExtrusionGrowZoomStart;
    if (range <= 0.0) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp((zoom - kBuildingExtrusionGrowZoomStart) / range, 0.0, 1.0));
}

} // namespace mbgl
