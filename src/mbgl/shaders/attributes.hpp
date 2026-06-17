#pragma once

#include <mbgl/gfx/attribute.hpp>
#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace attributes {

// Layout attributes

MBGL_DEFINE_ATTRIBUTE(int16_t, 2, pos);
MBGL_DEFINE_ATTRIBUTE(int16_t, 2, extrude);
MBGL_DEFINE_ATTRIBUTE(int16_t, 4, pos_offset);
MBGL_DEFINE_ATTRIBUTE(int16_t, 2, pos_normal);
MBGL_DEFINE_ATTRIBUTE(float, 3, projected_pos);
MBGL_DEFINE_ATTRIBUTE(int16_t, 4, pixeloffset);
MBGL_DEFINE_ATTRIBUTE(int16_t, 2, label_pos);
MBGL_DEFINE_ATTRIBUTE(int16_t, 2, anchor_pos);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 2, texture_pos);
MBGL_DEFINE_ATTRIBUTE(float, 1, fade_opacity);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 2, placed);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 3, size);
MBGL_DEFINE_ATTRIBUTE(float, 1, offset);
MBGL_DEFINE_ATTRIBUTE(float, 2, shift);

#if MLN_USE_FILL_EXTRUSION_INSTANCING
MBGL_DEFINE_ATTRIBUTE(uint16_t, 2, ed_discard);
#else
MBGL_DEFINE_ATTRIBUTE(int16_t, 4, normal_ed);
#endif

#if MLN_GL_FE_INSTANCING
// GL edge-indexed fill-extrusion instance attributes (Plan 1). One instance per outline
// vertex carries the current footprint endpoint (pos), the NEXT endpoint (pos1), and the
// smoothed wall normal at each endpoint (normal0/normal1) — GLES 3.0 cannot fetch the
// neighbor by gl_InstanceID, so they are precomputed. edgedistance wraps fill patterns.
MBGL_DEFINE_ATTRIBUTE(int16_t, 2, pos1);
MBGL_DEFINE_ATTRIBUTE(int16_t, 3, normal0);
MBGL_DEFINE_ATTRIBUTE(int16_t, 3, normal1);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 1, edgedistance);
#endif

template <typename T, std::size_t N>
struct data {
    using Type = gfx::AttributeType<T, N>;
    static constexpr auto name() { return "data"; }
};

// Paint attributes

MBGL_DEFINE_ATTRIBUTE(float, 2, color);
MBGL_DEFINE_ATTRIBUTE(float, 2, fill_color);
MBGL_DEFINE_ATTRIBUTE(float, 2, halo_color);
MBGL_DEFINE_ATTRIBUTE(float, 2, stroke_color);
MBGL_DEFINE_ATTRIBUTE(float, 2, outline_color);
MBGL_DEFINE_ATTRIBUTE(float, 1, opacity);
MBGL_DEFINE_ATTRIBUTE(float, 1, stroke_opacity);
MBGL_DEFINE_ATTRIBUTE(float, 1, blur);
MBGL_DEFINE_ATTRIBUTE(float, 1, radius);
MBGL_DEFINE_ATTRIBUTE(float, 1, width);
MBGL_DEFINE_ATTRIBUTE(float, 1, floorwidth);
MBGL_DEFINE_ATTRIBUTE(float, 1, height);
MBGL_DEFINE_ATTRIBUTE(float, 1, base);
MBGL_DEFINE_ATTRIBUTE(float, 1, gapwidth);
MBGL_DEFINE_ATTRIBUTE(float, 1, stroke_width);
MBGL_DEFINE_ATTRIBUTE(float, 1, halo_width);
MBGL_DEFINE_ATTRIBUTE(float, 1, halo_blur);
MBGL_DEFINE_ATTRIBUTE(float, 1, weight);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 4, pattern_to);
MBGL_DEFINE_ATTRIBUTE(uint16_t, 4, pattern_from);

} // namespace attributes

using PositionOnlyLayoutAttributes = TypeList<attributes::pos>;

} // namespace mbgl
