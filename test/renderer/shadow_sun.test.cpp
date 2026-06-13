#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/shadow_sun.hpp>
#include <mbgl/style/position.hpp>
#include <mbgl/style/types.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/math/angles.hpp>

#include <array>
#include <cmath>

using namespace mbgl;

namespace {
double length(const vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
style::Position makePosition(float radial, float azimuth, float polar) {
    // Position's ctor takes a non-const lvalue reference.
    std::array<float, 3> spherical{{radial, azimuth, polar}};
    return style::Position(spherical);
}
} // namespace

TEST(ShadowSun, ReturnsUnitDirection) {
    const auto pos = makePosition(1.5f, 210.0f, 45.0f);
    const vec3 dir = ShadowSun::direction(pos, style::LightAnchorType::Map, 0.0f);
    EXPECT_NEAR(length(dir), 1.0, 1e-5);
}

TEST(ShadowSun, MapAnchorIgnoresBearing) {
    const auto pos = makePosition(1.5f, 210.0f, 45.0f);
    const vec3 a = ShadowSun::direction(pos, style::LightAnchorType::Map, 0.0f);
    const vec3 b = ShadowSun::direction(pos, style::LightAnchorType::Map, util::deg2radf(90.0f));
    EXPECT_NEAR(a[0], b[0], 1e-6);
    EXPECT_NEAR(a[1], b[1], 1e-6);
    EXPECT_NEAR(a[2], b[2], 1e-6);
}

TEST(ShadowSun, ViewportAnchorRotatesWithBearing) {
    const auto pos = makePosition(1.5f, 210.0f, 45.0f);
    const vec3 a = ShadowSun::direction(pos, style::LightAnchorType::Viewport, 0.0f);
    const vec3 b = ShadowSun::direction(pos, style::LightAnchorType::Viewport, util::deg2radf(90.0f));
    // Viewport-anchored light follows the map bearing, exactly mirroring
    // FillExtrusionBucket::lightPosition (so shadows and FE shading share one sun),
    // so the direction must change with bearing.
    const double diff = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                  (a[2] - b[2]) * (a[2] - b[2]));
    EXPECT_GT(diff, 1e-3);
}

TEST(ShadowSun, OverheadSunPointsUp) {
    // polar 0 == straight overhead -> cartesian (0,0,radial) -> +Z.
    const auto pos = makePosition(1.0f, 0.0f, 0.0f);
    const vec3 dir = ShadowSun::direction(pos, style::LightAnchorType::Map, 0.0f);
    EXPECT_NEAR(dir[2], 1.0, 1e-4);
}
