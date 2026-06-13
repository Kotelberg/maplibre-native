#pragma once

#include <mbgl/util/vectors.hpp>
#include <mbgl/util/mat4.hpp>

#include <cstdint>
#include <vector>

namespace mbgl {

/// Pure math for fitting an orthographic light-space view-projection that covers a set
/// of world points. The full camera-frustum-to-ground fit lives in worldToLightClip()
/// (added in S1.T8); these primitives are unit-tested in isolation.
struct ShadowFrustum {
    struct Aabb {
        vec3 min;
        vec3 max;
    };

    /// Right-handed light view basis looking from the sun direction back at the origin.
    static mat4 lightView(const vec3& sunDir);

    /// AABB of `worldPoints` transformed into the sun's light space.
    static Aabb lightSpaceAabb(const vec3& sunDir, const std::vector<vec3>& worldPoints);

    /// Snap a world coordinate to a texel grid of size `texelWorldSize` (floor-based).
    static double texelSnap(double value, double texelWorldSize);

    /// Duplicate each ground point at z=0 and z=maxHeight so the light frustum also
    /// covers building tops (casters are extruded to height).
    static std::vector<vec3> heightExpand(const std::vector<vec3>& groundPoints, double maxHeight);

    /// Full fit: ortho(AABB) * lightView, optionally texel-snapping the AABB origin.
    static mat4 fit(const vec3& sunDir,
                    const std::vector<vec3>& worldPoints,
                    uint32_t mapSize,
                    bool texelSnapEnabled);
};

} // namespace mbgl
