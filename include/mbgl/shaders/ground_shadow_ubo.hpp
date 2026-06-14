#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) GroundShadowDrawableUBO {
    /*   0 */ std::array<float, 4 * 4> matrix;       // tile-local -> clip
    /*  64 */ std::array<float, 4 * 4> light_matrix; // tile-local -> light clip
    /* 128 */
};
static_assert(sizeof(GroundShadowDrawableUBO) == 8 * 16);

struct alignas(16) GroundShadowPropsUBO {
    /*  0 */ Color shadow_color;
    /* 16 */ float shadow_intensity;
    /* 20 */ float shadow_texel_size;
    /* 24 */ float shadow_bias;
    // UV-radial distance from the (centered, texel-snapped) light frustum where the cast
    // shadow begins fading to lit. Softens the bounded far edge so it reads as a graceful
    // distance fade instead of a hard cut-off line at steep pitch. 1.0 = no fade.
    /* 28 */ float shadow_fade_start;
    /* 32 */
};
static_assert(sizeof(GroundShadowPropsUBO) == 2 * 16);

} // namespace shaders
} // namespace mbgl
