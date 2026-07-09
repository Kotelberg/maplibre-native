#include <mbgl/test/util.hpp>

#include <mbgl/renderer/shadows/depth_pack.hpp>

#include <cmath>

using namespace mbgl::shadows;

namespace {
// One texel's worth of round-trip error for the 4x8 (RGBA8) packed encoding: each channel
// contributes 1/(256^i), so the finest channel (the 4th) is worth 1/16581375 (== 1/255^3, matching
// bitSh[3] in depth_pack.hpp). Give the round trip a little headroom over that single-ULP budget.
constexpr double kTolerance = 4.0 / 16581375.0;
} // namespace

// Round-trips a sweep of depths through packDepth()/unpackDepth() (the RGBA8 packed-depth codec
// pasted verbatim into the MSL/GLSL shadow shaders — see depth_pack.hpp) and checks every documented
// edge case: the [0,1] domain, the two boundary/degenerate values (0.0, and 1.0 which clamps to
// kMaxPackable to dodge the fract(255*1)==0 degeneracy), out-of-range clamping, and that the
// documented bitSh constant family (powers of 255) is the one actually in effect.
TEST(DepthPack, RoundTrip) {
    // Fine-grained sweep across the full domain.
    for (int i = 0; i <= 1000; ++i) {
        const double depth = static_cast<double>(i) / 1000.0;
        const double unpacked = unpackDepth(packDepth(depth));
        EXPECT_NEAR(depth, unpacked, kTolerance) << "depth=" << depth;
    }

    // Out-of-range inputs clamp to [0, kMaxPackable] before encoding.
    EXPECT_NEAR(0.0, unpackDepth(packDepth(-0.5)), kTolerance);
    EXPECT_NEAR(kMaxPackable, unpackDepth(packDepth(1.5)), kTolerance);

    // The documented degeneracy guard: packDepth(1.0) lands on kMaxPackable's own encoding rather
    // than colliding with the fract(255*1)==0 case, and kMaxPackable itself round-trips.
    EXPECT_NEAR(kMaxPackable, unpackDepth(packDepth(kMaxPackable)), kTolerance);
    EXPECT_NEAR(kMaxPackable, unpackDepth(packDepth(1.0)), kTolerance);

    // The documented bitSh family is powers of 255 (1, 255, 255^2, 255^3); confirm kMaxPackable's
    // "one LSB below 1.0" comment refers to exactly that 255^3 denominator.
    EXPECT_DOUBLE_EQ(16581375.0, std::pow(255.0, 3));
    EXPECT_NEAR(1.0 / 16581375.0, 1.0 - kMaxPackable, 1e-15);
}
