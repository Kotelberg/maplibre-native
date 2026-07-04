#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>
#include <mbgl/shaders/layer_ubo.hpp>

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

// On the INSTANCED fill-extrusion path the roof caster (ShadowDepthShader) draws the building ROOF
// (sharedTriangles over footprint verts, which carry ed_discard, NOT normal_ed): FE_INSTANCING makes
// it use a constant top flag t=1 and drop the absent normal_ed. The WALL caster
// (ShadowDepthInstancedShader, below) reads the same per-edge OutlineInstance SSBO the visible
// instanced FE uses (binding drawableSSBOStartId).
constexpr auto shadowDepthShaderPrelude =
#if MLN_USE_FILL_EXTRUSION_INSTANCING
    "#define FE_INSTANCING 1\n"
#else
    "#define FE_INSTANCING 0\n"
#endif
    R"(#define idShadowDepthDrawableUBO  drawableUBOStartId
#define idFillExtrusionInstancedDrawableUBO  drawableSSBOStartId)";

template <>
struct ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "ShadowDepthShader";

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    static const std::array<AttributeInfo, 3> attributes; // pos, base, height (roof; no normal_ed)
#else
    static const std::array<AttributeInfo, 4> attributes;
#endif
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position;
#if !FE_INSTANCING
layout(location = 1) in ivec4 in_normal_ed;
#endif

// Instanced roof caster: no normal_ed; base/height shift down to slots 1/2.
#if FE_INSTANCING
#if !defined(HAS_UNIFORM_u_base)
layout(location = 1) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 2) in vec2 in_height;
#endif
#else
#if !defined(HAS_UNIFORM_u_base)
layout(location = 2) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 3) in vec2 in_height;
#endif
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

    // t (top/bottom flag) selects height vs base for z, matching the FE vertex. On the instanced path
    // this caster draws the roof cap, which is always the top, so t is the constant 1.
#if FE_INSTANCING
    float t = 1.0;
#else
    float t = float(in_normal_ed.x & 1);
#endif
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

#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced WALL caster. The roof caster (ShadowDepthShader above) casts only the building ROOF on the
// instanced path (sharedTriangles is roof-only there) — so the cast shadow is the footprint projected
// from roof height, DETACHED from the building base. This shader casts the WALLS too: it reconstructs the
// extruded wall quads from the same per-edge OutlineInstance SSBO the visible FillExtrusionInstancedShader
// uses (static unit quad × per-instance footprint edge, t = in_position.y selects base vs height), and
// packs light-space depth like the roof caster. Together the roof+wall casters fill the full building
// volume in the shadow map, so ground shadows reattach to the base (matching the non-instanced GL path,
// whose sharedTriangles already includes walls).
template <>
struct ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "ShadowDepthInstancedShader";

    static const std::array<AttributeInfo, 1> attributes;         // static quad pos
    static const std::array<AttributeInfo, 5> instanceAttributes; // outline pos + ed_discard, color, base, height
    static constexpr std::array<TextureInfo, 0> textures{};

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position; // static unit quad: .x = edge endpoint {0,1}, .y = base/height {0,1}

// color (location 3) is kept in the metadata to anchor the interleaved instance paint buffer, but the
// caster never shades so it is not declared here. base/height are the data-driven instance attributes.
#if !defined(HAS_UNIFORM_u_base)
layout(location = 4) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 5) in vec2 in_height;
#endif

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idShadowDepthDrawableUBO) uniform ShadowDepthDrawableUBO {
    mat4 light_matrix;
    float base_t;
    float height_t;
    float u_base;
    float u_height;
} drawable;

struct OutlineInstance {
    int pos;
    uint ed_discard;
};

layout(std430, set = DRAWABLE_UBO_SET_INDEX, binding = idFillExtrusionInstancedDrawableUBO) readonly buffer FillExtrusionInstanceVector {
    OutlineInstance instance[];
} instanceVector;

layout(location = 0) out float frag_depth01;

void main() {
    // Discarded instance (the ring-closing edge): collapse to a degenerate point so it casts nothing.
    const vec2 instanceEd = unpack_uint(instanceVector.instance[gl_InstanceIndex].ed_discard);
    if (instanceEd.y > 0.0) {
        frag_depth01 = 0.0;
        gl_Position = vec4(0.0);
        return;
    }

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

    // Static quad: pos.y selects base(0)/height(1); pos.x selects the edge's near/far endpoint.
    float t = float(in_position.y);
    float z = (t != 0.0) ? height : base;
    vec4 worldLocal = vec4(unpack_int(instanceVector.instance[gl_InstanceIndex + in_position.x].pos), z, 1.0);
    vec4 clip = drawable.light_matrix * worldLocal;
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
    out_color = packDepth(frag_depth01);
}

)";
};
#endif // MLN_USE_FILL_EXTRUSION_INSTANCING

} // namespace shaders
} // namespace mbgl
