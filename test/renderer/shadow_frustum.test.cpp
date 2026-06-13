#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/shadow_frustum.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/vectors.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace mbgl;

TEST(ShadowFrustum, AabbExtentsForOverheadSun) {
    // Overhead sun: light XY are an orthonormal image of world XY, so axis extents
    // are preserved (sign/flip is convention-dependent, extents are not).
    const vec3 sunUp{0.0, 0.0, 1.0};
    const std::vector<vec3> pts = {{-10, -20, 0}, {30, 5, 0}, {0, 40, 0}};
    const ShadowFrustum::Aabb box = ShadowFrustum::lightSpaceAabb(sunUp, pts);
    EXPECT_NEAR(box.max[0] - box.min[0], 40.0, 1e-3);
    EXPECT_NEAR(box.max[1] - box.min[1], 60.0, 1e-3);
}

TEST(ShadowFrustum, TexelSnapQuantizes) {
    EXPECT_NEAR(ShadowFrustum::texelSnap(3.3, 2.0), 2.0, 1e-9);
    EXPECT_NEAR(ShadowFrustum::texelSnap(4.0, 2.0), 4.0, 1e-9);
    EXPECT_NEAR(ShadowFrustum::texelSnap(-1.1, 2.0), -2.0, 1e-9);
}

TEST(ShadowFrustum, HeightExpandAddsRaisedPoints) {
    const std::vector<vec3> ground = {{0, 0, 0}, {10, 10, 0}};
    const std::vector<vec3> out = ShadowFrustum::heightExpand(ground, 50.0);
    ASSERT_EQ(out.size(), 4u);
    int raised = 0;
    for (const auto& p : out) {
        if (std::abs(p[2] - 50.0) < 1e-9) ++raised;
    }
    EXPECT_EQ(raised, 2);
}

TEST(ShadowFrustum, OrthoMapsAllPointsIntoClipXY) {
    const vec3 sunUp{0.0, 0.0, 1.0};
    const std::vector<vec3> ground = {{-10, -20, 0}, {30, 5, 0}, {0, 40, 0}};
    const std::vector<vec3> pts = ShadowFrustum::heightExpand(ground, 25.0);
    const mat4 m = ShadowFrustum::fit(sunUp, pts, /*mapSize=*/1024, /*texelSnapEnabled=*/false);
    for (const auto& p : pts) {
        vec4 clip;
        matrix::transformMat4(clip, vec4{{p[0], p[1], p[2], 1.0}}, m);
        EXPECT_GE(clip[0], -1.0 - 1e-6);
        EXPECT_LE(clip[0], 1.0 + 1e-6);
        EXPECT_GE(clip[1], -1.0 - 1e-6);
        EXPECT_LE(clip[1], 1.0 + 1e-6);
    }
}
