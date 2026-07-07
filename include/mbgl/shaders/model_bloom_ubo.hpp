#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) ModelBloomDrawableUBO {
    /*  0 */ std::array<float, 4> color;  // rgb = glow colour, a = intensity
    /* 16 */ std::array<float, 2> texel;  // 1 / mask size
    /* 24 */ float radius;                // blur radius in texels
    /* 28 */ float pad;
    /* 32 */
};
static_assert(sizeof(ModelBloomDrawableUBO) == 2 * 16);

} // namespace shaders
} // namespace mbgl
