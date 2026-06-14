#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

// Self-contained UBOs for the shadow-receiving fill-extrusion variant. Deliberately separate
// from FillExtrusion{Drawable,Props}UBO so the stock fill-extrusion shader/UBO/tweaker stay
// byte-identical when shadows are off. Non-consolidated (one drawable UBO per drawable).
struct alignas(16) FillExtrusionShadowDrawableUBO {
    /*   0 */ std::array<float, 4 * 4> matrix;       // tile-local -> clip
    /*  64 */ std::array<float, 4 * 4> light_matrix; // tile-local -> light clip
    /* 128 */ float base_t;
    /* 132 */ float height_t;
    /* 136 */ float color_t;
    /* 140 */ float pad1;
    /* 144 */
};
static_assert(sizeof(FillExtrusionShadowDrawableUBO) == 9 * 16);

struct alignas(16) FillExtrusionShadowPropsUBO {
    /*  0 */ Color color;
    /* 16 */ std::array<float, 4> light_color_pad;
    /* 32 */ std::array<float, 4> light_position_base; // xyz = light dir, w = base (uniform path)
    /* 48 */ float height;
    /* 52 */ float light_intensity;
    /* 56 */ float vertical_gradient;
    /* 60 */ float opacity;
    /* 64 */ float shadow_intensity;
    /* 68 */ float shadow_texel_size; // 1.0 / shadow map size
    /* 72 */ float shadow_bias;
    /* 76 */ float shadow_slope_bias; // extra bias scaled by (1 - n·L); kills self-shadowing
    /* 80 */
};
static_assert(sizeof(FillExtrusionShadowPropsUBO) == 5 * 16);

} // namespace shaders
} // namespace mbgl
