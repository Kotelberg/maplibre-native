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
