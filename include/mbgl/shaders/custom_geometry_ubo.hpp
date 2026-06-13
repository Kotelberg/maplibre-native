#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) CustomGeometryDrawableUBO {
    /*   0 */ std::array<float, 4 * 4> matrix;
    /*  64 */ Color color;
    /*  80 */ Color highlight;               // rgb = fresnel rim colour, a = intensity (0 = off)
    /*  96 */ std::array<float, 4> viewAxis;  // xyz = view dir in model space, w = rim power
    /* 112 */
};
static_assert(sizeof(CustomGeometryDrawableUBO) == 7 * 16);

} // namespace shaders
} // namespace mbgl
