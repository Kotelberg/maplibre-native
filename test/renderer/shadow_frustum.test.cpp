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

// CORE INVARIANT (the loop-breaker): a world-anchored directional light must not move when the
// camera ROTATES. For a fixed look-at center / zoom / pitch and a fixed world (map-anchored) sun,
// worldToLightClip must be identical across bearing 0 / 90 / 200. The coverage radius is a scalar
// (the farthest visible ground distance) applied as a SYMMETRIC square around the look-at point, so
// it is bearing-invariant by construction; a view-frustum fit would FAIL this (it rotates with the
// camera, so shadow texels crawl as you turn). This is the "shadows don't move as I rotate" guarantee.
TEST(ShadowFrustum, WorldToLightClipIsBearingInvariant) {
    const vec3 sunDir{0.3, 0.5, 0.81};
    const LatLng center{50.4501, 30.5234}; // Kyiv (Maidan)
    const uint32_t mapSize = 2048;

    const mat4 m0 = computeWorldToLightClip(stateAt(16.0, 55.0, 0.0, center), sunDir, mapSize);
    const mat4 m90 = computeWorldToLightClip(stateAt(16.0, 55.0, 90.0, center), sunDir, mapSize);
    const mat4 m200 = computeWorldToLightClip(stateAt(16.0, 55.0, 200.0, center), sunDir, mapSize);

    EXPECT_LT(maxAbsDiff(m0, m90), 1e-6) << "light frustum changed when rotating bearing 0->90 (shadows would move)";
    EXPECT_LT(maxAbsDiff(m0, m200), 1e-6) << "light frustum changed when rotating bearing 0->200 (shadows would move)";
}

// The frustum must be well-formed (finite, non-singular) across the pitch range — a malformed /
// non-finite matrix is what produced earlier coverage failures.
TEST(ShadowFrustum, WorldToLightClipIsFiniteAcrossPitch) {
    const vec3 sunDir{0.3, 0.5, 0.81};
    const LatLng center{50.4501, 30.5234};
    const uint32_t mapSize = 2048;
    for (double pitch : {0.0, 30.0, 60.0}) {
        const mat4 m = computeWorldToLightClip(stateAt(16.0, pitch, 0.0, center), sunDir, mapSize);
        for (int i = 0; i < 16; ++i) {
            EXPECT_TRUE(std::isfinite(m[i])) << "non-finite worldToLightClip entry at pitch " << pitch;
        }
    }
}

// Padding-aware frustum: under CAMERA_PADDING / edge insets (e.g. a bottom-sheet) the light frustum
// must center on the PADDED look-at, not the geometric screen center. With NO insets the matrix is
// unchanged (the no-padding path stays exactly as verified); with insets it shifts but stays finite.
TEST(ShadowFrustum, PaddingCentersOnLookAt) {
    const vec3 sunDir{0.3, 0.5, 0.81};
    const LatLng center{50.4501, 30.5234};
    const uint32_t mapSize = 1024;

    const mat4 plain = computeWorldToLightClip(stateAt(16.0, 55.0, 0.0, center), sunDir, mapSize);

    // HataHub-like bottom-sheet insets: EdgeInsets(top, left, bottom, right) = (88, 24, 130, 24).
    Transform t;
    t.resize({512, 512});
    t.jumpTo(CameraOptions()
                 .withCenter(center)
                 .withZoom(16.0)
                 .withPitch(55.0)
                 .withBearing(0.0)
                 .withPadding(EdgeInsets{88, 24, 130, 24}));
    const TransformState padded = t.getState();
    const mat4 mPadded = computeWorldToLightClip(padded, sunDir, mapSize);

    // The asymmetric vertical insets shift the look-at up-screen (top<bottom ⇒ center offset y<0).
    const EdgeInsets ins = padded.getEdgeInsets();
    EXPECT_LT(0.5 * (ins.top() - ins.bottom()), 0.0);
    // Frustum stays finite under padding (no NaN/regression).
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(std::isfinite(mPadded[i])) << "non-finite padded worldToLightClip entry " << i;
    }
    // Padding actually moves the frustum (it is now centered on the padded look-at, not ignored).
    EXPECT_GT(maxAbsDiff(plain, mPadded), 0.0);

    // No insets ⇒ getCenterOffset()==(0,0) ⇒ identical to the geometric-center path (no regression).
    const mat4 noInset = computeWorldToLightClip(stateAt(16.0, 55.0, 0.0, center), sunDir, mapSize);
    EXPECT_EQ(maxAbsDiff(plain, noInset), 0.0);
}
