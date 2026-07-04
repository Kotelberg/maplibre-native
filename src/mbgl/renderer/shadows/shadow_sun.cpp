#include <mbgl/renderer/shadows/shadow_sun.hpp>

#include <mbgl/style/position.hpp>
#include <mbgl/util/mat3.hpp>

#include <array>
#include <cmath>

namespace mbgl {

vec3 ShadowSun::direction(const style::Position& position,
                          style::LightAnchorType anchor,
                          float bearingRadians) {
    // Spherical (radial, azimuth, polar) -> cartesian, same as Position::getCartesian().
    const std::array<float, 3> cart = position.getCartesian();
    vec3f dir{{cart[0], cart[1], cart[2]}};

    if (anchor == style::LightAnchorType::Viewport) {
        mat3 m;
        matrix::identity(m);
        matrix::rotate(m, m, -static_cast<double>(bearingRadians));
        // NB: the aliased (out == in) transformMat3f call is intentional — it mirrors
        // FillExtrusionBucket::lightPosition exactly, so shadows and FE shading share the
        // same bearing-rotated sun. (That shared call has a known aliasing quirk for
        // viewport+bearing; matching it keeps the two consistent. See LEARNINGS.)
        matrix::transformMat3f(dir, dir, m);
    }

    const double len = std::sqrt(static_cast<double>(dir[0]) * dir[0] +
                                 static_cast<double>(dir[1]) * dir[1] +
                                 static_cast<double>(dir[2]) * dir[2]);
    const double inv = len > 0.0 ? 1.0 / len : 0.0;
    return {dir[0] * inv, dir[1] * inv, dir[2] * inv};
}

} // namespace mbgl
