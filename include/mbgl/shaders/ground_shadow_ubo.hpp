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
    // View-depth (camera-distance) fade: the cast shadow fades from full at depth_fade_start
    // to none at depth_fade_end, both expressed in clip-space w (perspective view-distance, in
    // world/mercator-px units). This makes ANY far cut-off (frustum edge, caster-tile horizon,
    // padding miscentering) read as a smooth near→far fade instead of a hard horizontal line at
    // steep pitch, WITHOUT touching the near field (which has small w). depth_fade_end<=0 = off.
    /* 32 */ float depth_fade_start;
    /* 36 */ float depth_fade_end;
    /* 40 */ float pad0;
    /* 44 */ float pad1;
    /* 48 */
};
static_assert(sizeof(GroundShadowPropsUBO) == 3 * 16);

} // namespace shaders
} // namespace mbgl
