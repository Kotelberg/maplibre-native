#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/shadow_frustum.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>
#include <mbgl/map/transform.hpp>
#include <mbgl/map/camera.hpp>
#include <mbgl/util/geo.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/size.hpp>
#include <mbgl/util/vectors.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace mbgl;

namespace {

// Max element-wise absolute difference between two 4x4 matrices.
double maxAbsDiff(const mat4& a, const mat4& b) {
    double d = 0.0;
    for (int i = 0; i < 16; ++i) {
        d = std::max(d, std::abs(a[i] - b[i]));
    }
    return d;
}

// Build a TransformState for a given camera. pitch/bearing in degrees.
TransformState stateAt(double zoom, double pitchDeg, double bearingDeg, LatLng center, Size size = {512, 512}) {
    Transform t;
    t.resize(size);
    t.jumpTo(CameraOptions()
                 .withCenter(center)
                 .withZoom(zoom)
                 .withPitch(pitchDeg)
                 .withBearing(bearingDeg));
    return t.getState();
}

} // namespace

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

// A building top corner and its analytic ground shadow tip lie on the same light ray, so the
// orthographic light projection must map them to the SAME clip-space xy (the shadow lands exactly
// under the geometry that casts it). This is the projection-level alignment guarantee; it must hold
// before and after the frustum rewrite. Tip = top - (H / sunDir.z) * sunDir (travel along -sun to z=0).
TEST(ShadowFrustum, ShadowTipAlignsWithCaster) {
    const vec3 sunDir{0.3, 0.5, 0.81}; // oblique world-fixed sun
    const double H = 60.0;
    const vec3 top{120.0, -40.0, H};
    const double t = H / sunDir[2];
    const vec3 tip{top[0] - t * sunDir[0], top[1] - t * sunDir[1], 0.0};

    // Footprint covering the caster + its shadow so both land inside the fitted frustum.
    const std::vector<vec3> pts = ShadowFrustum::heightExpand({{120.0, -40.0, 0.0}, tip, {0, 0, 0}}, H);
    const mat4 m = ShadowFrustum::fit(sunDir, pts, /*mapSize=*/2048, /*texelSnapEnabled=*/false);

    vec4 cTop, cTip;
    matrix::transformMat4(cTop, vec4{{top[0], top[1], top[2], 1.0}}, m);
    matrix::transformMat4(cTip, vec4{{tip[0], tip[1], tip[2], 1.0}}, m);
    EXPECT_NEAR(cTop[0] / cTop[3], cTip[0] / cTip[3], 1e-4) << "shadow tip x must match caster top x";
    EXPECT_NEAR(cTop[1] / cTop[3], cTip[1] / cTip[3], 1e-4) << "shadow tip y must match caster top y";
    // The top corner is nearer the light than the ground tip => strictly smaller packed depth.
    EXPECT_LT(cTop[2] / cTop[3], cTip[2] / cTip[3]) << "caster top must be nearer the light than its shadow";
}

// CORE INVARIANT (the loop-breaker): a world-anchored directional light does not depend on camera
// pitch. For a fixed look-at center / zoom / bearing and a fixed world sun, worldToLightClip must be
// identical at pitch 0, 30 and 60. The legacy per-frame view-frustum-fit FAILS this (the footprint
// is the camera's view trapezoid, which balloons with pitch); the stable fixed-radius fit PASSES it.
TEST(ShadowFrustum, WorldToLightClipIsPitchInvariant) {
    const vec3 sunDir{0.3, 0.5, 0.81};
    const LatLng center{50.4501, 30.5234}; // Kyiv (Maidan)
    const uint32_t mapSize = 2048;

    const mat4 m0 = computeWorldToLightClip(stateAt(16.0, 0.0, 0.0, center), sunDir, mapSize);
    const mat4 m30 = computeWorldToLightClip(stateAt(16.0, 30.0, 0.0, center), sunDir, mapSize);
    const mat4 m60 = computeWorldToLightClip(stateAt(16.0, 60.0, 0.0, center), sunDir, mapSize);

    EXPECT_LT(maxAbsDiff(m0, m30), 1e-6) << "light frustum changed between pitch 0 and 30 (camera-coupled)";
    EXPECT_LT(maxAbsDiff(m0, m60), 1e-6) << "light frustum changed between pitch 0 and 60 (camera-coupled)";
}

// The world->light-clip scale (shadow-map world coverage) must be a function of zoom and the sun
// only, NOT of the look-at center: panning must not resize the frustum (else the world->texel scale
// drifts and shadows "swim"). Probe operationally: a fixed world delta projects to the same clip-xy
// length regardless of center. The legacy view-fit FAILS this; the fixed-radius fit PASSES.
TEST(ShadowFrustum, WorldToLightClipScaleIsCenterInvariant) {
    const vec3 sunDir{0.3, 0.5, 0.81};
    const uint32_t mapSize = 2048;
    const LatLng centerA{50.4501, 30.5234};
    const LatLng centerB{50.4530, 30.5300}; // panned a few hundred metres

    const mat4 mA = computeWorldToLightClip(stateAt(16.0, 45.0, 0.0, centerA), sunDir, mapSize);
    const mat4 mB = computeWorldToLightClip(stateAt(16.0, 45.0, 0.0, centerB), sunDir, mapSize);

    // Clip-xy length of a fixed (1000,0,0) world delta under each matrix (translation cancels).
    auto clipDeltaLen = [](const mat4& m) {
        return std::hypot(m[0] * 1000.0, m[1] * 1000.0); // column 0 (x-basis) projected to clip xy
    };
    const double la = clipDeltaLen(mA);
    const double lb = clipDeltaLen(mB);
    EXPECT_GT(la, 0.0);
    EXPECT_NEAR(la, lb, la * 1e-3) << "frustum scale changed with pan (shadow texels would swim)";
}
