#pragma once

#include <algorithm>

namespace mbgl {

// Integer-aligned grow band (14→15). The style-side fill-extrusion height ramp
// must use these exact stops, but MapLibre quantizes a composite (zoom+feature)
// paint expression to INTEGER zoom levels — so the style only renders the ramp
// faithfully when both stops land on integers. A sub-integer end (the old 15.2)
// rendered as 15→16 on the style side while this C++ ramp stayed 15→15.2,
// desyncing models from the buildings around them. 14→15 is integer-aligned, so
// the per-frame C++ ramp here and the quantized style ramp agree exactly. Keep
// these in lockstep with BUILDING_EXTRUSION_ZOOM in
// packages/backend/lib/map-style/building-extrusion.ts.
constexpr float kBuildingExtrusionGrowZoomStart = 14.0f;
constexpr float kBuildingExtrusionGrowZoomEnd = 15.0f;
// Model alpha fade covers the first ~60% of the grow window (mesh is collapsed /
// z-fighting near grow≈0), same proportion as the old 15.0→15.12 over 15→15.2.
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
