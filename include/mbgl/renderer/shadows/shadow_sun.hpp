#pragma once

#include <mbgl/util/vectors.hpp>
#include <mbgl/style/types.hpp>

namespace mbgl {
namespace style {
class Position;
} // namespace style

/// Pure math: the normalized world-space direction from a surface toward the sun,
/// matching fill-extrusion's lighting convention (used as dot(normal, dir)). Reuses the
/// same spherical->cartesian + viewport-bearing-rotation behaviour as
/// FillExtrusionBucket::lightPosition, so building shading and shadows agree on the sun.
struct ShadowSun {
    static vec3 direction(const style::Position& position,
                          style::LightAnchorType anchor,
                          float bearingRadians);
};

} // namespace mbgl
