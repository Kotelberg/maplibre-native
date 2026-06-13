#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/depth_pack.hpp>

#include <array>

using namespace mbgl;

TEST(DepthPack, RoundTrips) {
    for (double d : {0.0, 0.001, 0.25, 0.5, 0.7331, 0.999, 1.0}) {
        const std::array<float, 4> rgba = shadows::packDepth(d);
        EXPECT_NEAR(shadows::unpackDepth(rgba), d, 2e-6) << "depth " << d;
    }
}

TEST(DepthPack, OrderingPreserved) {
    EXPECT_LT(shadows::unpackDepth(shadows::packDepth(0.3)),
              shadows::unpackDepth(shadows::packDepth(0.6)));
}

TEST(DepthPack, WhiteClearReadsAsFar) {
    // The caster target is cleared to white == farthest; it must read >= any real depth.
    const std::array<float, 4> white{{1.f, 1.f, 1.f, 1.f}};
    EXPECT_GE(shadows::unpackDepth(white), shadows::unpackDepth(shadows::packDepth(0.9999)));
}
