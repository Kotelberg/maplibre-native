#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>

namespace mbgl {
namespace shaders {

// Caster pass: render every fill-extrusion building from the sun's point of view into an
// offscreen RGBA8 target, packing each fragment's [0,1] light-space depth. Mirrors the GLES/Metal
// ShadowDepthShader. Vulkan specifics vs GL:
//   * Vulkan NDC z is already [0,1] (like Metal), so — unlike GL — we do NOT remap gl_Position.z
//     ([-1,1]) ; the light_matrix bakes the [0,1] remap and clip.z/clip.w is the packed metric.
//   * The caster renders into the offscreen light-space target, so it must NOT call
//     applySurfaceTransform() (no Vulkan y-flip / no Android surface rotation). The receivers
//     compute their shadow-space uv from the SAME light_matrix without a uv.y flip, so the
//     rasterize/sample conventions cancel (the GLES convention) and the texel lookup aligns.
//   * Reuses the fill-extrusion vertex-attribute ids (pos/normal_ed/base/height) so the FE bucket
//     binders feed this caster; only normal_ed.x's LSB (the top/bottom flag) is consumed.

constexpr auto shadowDepthShaderPrelude = R"(#define idShadowDepthDrawableUBO  drawableUBOStartId)";

template <>
struct ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "ShadowDepthShader";

    static const std::array<AttributeInfo, 4> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position;
layout(location = 1) in ivec4 in_normal_ed;

#if !defined(HAS_UNIFORM_u_base)
layout(location = 2) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 3) in vec2 in_height;
#endif

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idShadowDepthDrawableUBO) uniform ShadowDepthDrawableUBO {
    mat4 light_matrix;
    float base_t;
    float height_t;
    float u_base;
    float u_height;
} drawable;

// The [0,1] light-space depth metric the receiver compares against.
layout(location = 0) out float frag_depth01;

void main() {
    // Match the visible FE / receiver vertex EXACTLY so the caster co-locates with the building you
    // see: same per-vertex interpolation factor (NOT a hardcoded 0.0) in the data-driven branch.
#if defined(HAS_UNIFORM_u_base)
    float base = max(drawable.u_base, 0.0);
#else
    float base = max(unpack_mix_float(in_base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    float height = max(drawable.u_height, 0.0);
#else
    float height = max(unpack_mix_float(in_height, drawable.height_t), 0.0);
#endif

    // t (top/bottom flag) selects height vs base for z, matching the FE vertex.
    float t = float(in_normal_ed.x & 1);
    vec4 clip = drawable.light_matrix * vec4(in_position, t > 0.0 ? height : base, 1.0);
    // Pack the [0,1] light-space depth (matches the receiver's ndc.z metric). Vulkan NDC z is
    // already [0,1], so the position passes through unmodified (no GL [-1,1] remap, no surface
    // transform — this is the offscreen light-space pass).
    frag_depth01 = clip.z / clip.w;
    gl_Position = clip;
}

)";

    static constexpr auto fragment = R"(

layout(location = 0) in float frag_depth01;
layout(location = 0) out vec4 out_color;

vec4 packDepth(float depth) {
    const float maxPackable = 1.0 - 1.0 / 16581375.0;
    depth = clamp(depth, 0.0, maxPackable);
    const vec4 bitSh = vec4(1.0, 255.0, 65025.0, 16581375.0);
    const vec4 mask  = vec4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    vec4 enc = fract(bitSh * depth);
    enc -= enc.yzww * mask;
    return enc;
}

void main() {
    // Pack the [0,1] light-space depth — the exact metric the receiver compares against.
    out_color = packDepth(frag_depth01);
}

)";
};

} // namespace shaders
} // namespace mbgl
