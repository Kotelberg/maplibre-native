#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/shadow_tweakers.hpp>
#include <mbgl/map/transform.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/util/geo.hpp>

#include <cmath>

using namespace mbgl;

namespace {
bool allFinite(const mat4& m) {
    for (double v : m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}
} // namespace

// Regression guard for the D3 grey-roof fix. A freshly-constructed ShadowFrustumState carries the
// sentinel cachedZoom (-1.0) and valid=false. refreshShadowFrustum() must REFIT it (clearing the
// sentinel) rather than feed the sentinel into the per-frame rescale: exp2(zoom - (-1)) is a
// ~5-orders-of-magnitude scale that would collapse the sampled light matrices into a degenerate
// projection (the "liveS=86475" garbage that produced a uniform grey roof wash on device). On a
// refit frame the live rescale factor is exactly 1, so liveCascades must equal the base cascades.
TEST(ShadowFrustumCache, SentinelCacheRefitsWithoutGarbageRescale) {
    Transform transform;
    transform.resize({1024, 768});
    transform.jumpTo(CameraOptions()
                         .withCenter(LatLng{50.4501, 30.5234})
                         .withZoom(15.4)
                         .withPitch(55.0)); // pitch in degrees
    const TransformState& state = transform.getState();

    // A mid-morning sun direction (non-overhead so the light basis is a real rotation).
    const vec3 sunDir{{0.4, 0.3, -0.86}};

    ShadowFrustumState fs; // fresh: cachedZoom == -1.0, valid == false, shadowMapUsable == false
    ASSERT_FALSE(fs.valid);
    ASSERT_DOUBLE_EQ(-1.0, fs.cachedZoom);
    ASSERT_FALSE(fs.shadowMapUsable);

    const bool refit = refreshShadowFrustum(fs, state, sunDir, /*mapSize=*/1024,
                                            /*activeCascades=*/1, /*split=*/0.4f);

    // An invalid (sentinel) cache must force a refit and clear the sentinel to the live zoom.
    EXPECT_TRUE(refit);
    EXPECT_TRUE(fs.valid);
    EXPECT_DOUBLE_EQ(state.getZoom(), fs.cachedZoom);

    // refreshShadowFrustum must NOT flip shadowMapUsable — that flag is the orchestrator's per-frame
    // snapshot of prior-frame cache validity (the receiver runs before the caster pass), so a fresh
    // fit alone must not unlock shadows this frame; the receiver stays dark-safe until a later frame.
    EXPECT_FALSE(fs.shadowMapUsable);

    // On a refit frame the rescale factor is exp2(zoom - cachedZoom) == 1, so the sampled matrices
    // equal the base matrices exactly. If the sentinel leaked into the rescale this would be a wild
    // scale instead, and the cascades would not match.
    ASSERT_EQ(fs.cascades.size(), fs.liveCascades.size());
    ASSERT_EQ(1u, fs.cascades.size());
    for (std::size_t c = 0; c < fs.cascades.size(); ++c) {
        EXPECT_TRUE(allFinite(fs.cascades[c])) << "cascade " << c;
        EXPECT_TRUE(allFinite(fs.liveCascades[c])) << "liveCascade " << c;
        for (std::size_t i = 0; i < 16; ++i) {
            EXPECT_DOUBLE_EQ(fs.cascades[c][i], fs.liveCascades[c][i]) << "cascade " << c << " elem " << i;
        }
    }
}

namespace {
// Mirror of shadow_tweakers.cpp's shadowHeightFade: buildings rise from flat to full height over the
// [14,15] grow band, and the caster/receiver interpolate their height by this factor. The sticky
// cache's height-drift refit keys off exactly this, so the test reads the same curve.
float heightFade(double zoom) {
    const double t = (zoom - 14.0) / (15.0 - 14.0);
    return static_cast<float>(t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
}
// Must match kHeightFadeRefit in refreshShadowFrustum.
constexpr float kHeightFadeRefit = 0.1f;
} // namespace

// Regression guard for the Metal pinch-zoom "shadow paints over the building" bug. During a zoom-out
// held on the sticky cache, the caster depth map keeps the height the casters were rendered at
// (cachedZoom), while the fill-extrusion receiver renders every frame at the LIVE (interpolated)
// height. The per-frame liveCascades rescale is a uniform world-space scale — it aligns a
// fixed-height building through a zoom but cannot correct a building whose height CHANGES with zoom
// across the [14,15] grow band. So a stale-height caster ends up self-shadowing the live roof (grey
// wash) until the next refit. The fix refits whenever the height-interp factor drifts past
// kHeightFadeRefit, keeping the caster height within tolerance of the live receiver height.
TEST(ShadowFrustumCache, GrowBandZoomOutTracksLiveBuildingHeight) {
    Transform transform;
    transform.resize({1024, 768});
    const vec3 sunDir{{0.4, 0.3, -0.86}};

    ShadowFrustumState fs;
    // Fit at the top of the grow band (buildings fully grown, fade == 1).
    transform.jumpTo(CameraOptions().withCenter(LatLng{50.4501, 30.5234}).withZoom(15.0).withPitch(55.0));
    ASSERT_TRUE(refreshShadowFrustum(fs, transform.getState(), sunDir, 1024, 1, 0.4f));

    // Zoom OUT through the grow band toward flat buildings in small steps, as an active pinch would.
    bool sawRefit = false;
    for (double zoom = 14.95; zoom >= 14.0; zoom -= 0.05) {
        transform.jumpTo(CameraOptions().withZoom(zoom));
        const bool refit = refreshShadowFrustum(fs, transform.getState(), sunDir, 1024, 1, 0.4f);
        sawRefit = sawRefit || refit;

        // The caster height baked at cachedZoom must stay within the interp tolerance of the live
        // height. Before the fix cachedZoom stuck at 15.0, so by zoom 14.0 the drift reached the full
        // 1.0 fade — the grey-wash regime. A tiny slack absorbs float rounding on the fade curve.
        EXPECT_LE(std::abs(heightFade(zoom) - heightFade(fs.cachedZoom)), kHeightFadeRefit + 1e-4f)
            << "height drift too large at zoom " << zoom << " (cachedZoom " << fs.cachedZoom << ")";
    }
    // The mechanism must actually engage (not vacuously pass by never entering the band).
    EXPECT_TRUE(sawRefit);
}

// A zoom held ABOVE the grow band (buildings already fully grown, fade flat at 1) must NOT trip the
// new height-drift refit — the sticky-cache pinch-flicker optimization is preserved everywhere except
// the active grow band. Only the first fit refits; the small in-place zoom-out reuses the cached map.
TEST(ShadowFrustumCache, AboveGrowBandZoomOutReusesCache) {
    Transform transform;
    transform.resize({1024, 768});
    const vec3 sunDir{{0.4, 0.3, -0.86}};

    ShadowFrustumState fs;
    transform.jumpTo(CameraOptions().withCenter(LatLng{50.4501, 30.5234}).withZoom(17.0).withPitch(55.0));
    ASSERT_TRUE(refreshShadowFrustum(fs, transform.getState(), sunDir, 1024, 1, 0.4f));

    // Small in-place zoom-out, both endpoints above the band → identical height fade → height trigger
    // silent. (Coverage/zoom-in triggers are unaffected by this change.)
    ASSERT_FLOAT_EQ(heightFade(17.0), heightFade(16.7));
    transform.jumpTo(CameraOptions().withZoom(16.7));
    EXPECT_FALSE(refreshShadowFrustum(fs, transform.getState(), sunDir, 1024, 1, 0.4f));
}
