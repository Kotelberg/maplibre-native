#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) ShadowDepthDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> light_matrix; // per-tile: tile-local -> light clip
    /* 64 */
};
static_assert(sizeof(ShadowDepthDrawableUBO) == 4 * 16);

} // namespace shaders
} // namespace mbgl
