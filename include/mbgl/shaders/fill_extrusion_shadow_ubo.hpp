#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

#include <array>
#include <cstdint>

namespace mbgl {
namespace shaders {

// Self-contained UBOs for the shadow-receiving fill-extrusion variant. Deliberately separate
// from FillExtrusion{Drawable,Props}UBO so the stock fill-extrusion shader/UBO/tweaker stay
// byte-identical when shadows are off. Non-consolidated (one drawable UBO per drawable).
// 4 == kMaxShadowCascades (mbgl/renderer/shadows/shadow_pass.hpp). MUST stay in lock-step with the
// MSL `FillExtrusionShadowDrawableUBO` in mtl/fill_extrusion_shadow.hpp (same field order + size).
struct alignas(16) FillExtrusionShadowDrawableUBO {
    /*   0 */ std::array<float, 4 * 4> matrix; // tile-local -> clip
    // tile-local -> light clip, one matrix per concentric cascade (near→far); only the first
    // `cascade_count` are valid.
    /*  64 */ std::array<std::array<float, 4 * 4>, 4> light_matrix;
    /* 320 */ float base_t;
    /* 324 */ float height_t;
    /* 328 */ float color_t;
    /* 332 */ std::int32_t cascade_count;
    /* 336 */ float pad0;
    /* 340 */ float pad1;
    /* 344 */ float pad2;
    /* 348 */ float pad3;
    /* 352 */
};
static_assert(sizeof(FillExtrusionShadowDrawableUBO) == 22 * 16);

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
