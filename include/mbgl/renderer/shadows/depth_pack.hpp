#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mbgl {
namespace shadows {

// One LSB below 1.0 for the 4x8 encoding. packDepth(1.0) maps here so it round-trips
// (avoids the fract(255*1)==0 degeneracy) and stays below the white far-clear sentinel.
inline constexpr double kMaxPackable = 1.0 - 1.0 / 16581375.0;

/// Pack a linear depth in [0,1] into RGBA8. Mirrors the MSL/GLSL packDepth() pasted into
/// the shadow shaders — keep the two in sync (guarded by the round-trip unit test in
/// test/renderer/depth_pack.test.cpp).
inline std::array<float, 4> packDepth(double depth) {
    const double d = std::min(std::max(depth, 0.0), kMaxPackable);
    const std::array<double, 4> bitSh{{1.0, 255.0, 65025.0, 16581375.0}};
    const std::array<double, 4> mask{{1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0}};
    std::array<double, 4> enc{};
    for (int i = 0; i < 4; ++i) {
        enc[i] = std::fmod(bitSh[i] * d, 1.0);
    }
    return {static_cast<float>(enc[0] - enc[1] * mask[0]),
            static_cast<float>(enc[1] - enc[2] * mask[1]),
            static_cast<float>(enc[2] - enc[3] * mask[2]),
            static_cast<float>(enc[3])};
}

inline double unpackDepth(const std::array<float, 4>& rgba) {
    const std::array<double, 4> bitSh{{1.0, 1.0 / 255.0, 1.0 / 65025.0, 1.0 / 16581375.0}};
    return rgba[0] * bitSh[0] + rgba[1] * bitSh[1] + rgba[2] * bitSh[2] + rgba[3] * bitSh[3];
}

} // namespace shadows
} // namespace mbgl
