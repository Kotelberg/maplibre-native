#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) ShadowDepthDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> light_matrix; // per-tile: tile-local -> light clip
    // Zoom interpolation factors + constant fallbacks for base/height, so the caster
    // extrudes to the SAME world height as the visible building & receiver. Without
    // these the caster hardcoded factor 0.0 (z15 stop = 0 height at fractional zoom)
    // => shadow cast from a ~flat building => misaligned/too-short ground shadows.
    /* 64 */ float base_t;
    /* 68 */ float height_t;
    /* 72 */ float u_base;   // constant base used when HAS_UNIFORM_u_base
    /* 76 */ float u_height; // constant height used when HAS_UNIFORM_u_height
    /* 80 */
};
static_assert(sizeof(ShadowDepthDrawableUBO) == 5 * 16);

} // namespace shaders
} // namespace mbgl
