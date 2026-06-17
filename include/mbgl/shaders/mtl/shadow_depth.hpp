#pragma once

#include <mbgl/shaders/shadow_depth_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>
#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

// On the INSTANCED fill-extrusion path the caster draws the building ROOF (the sharedTriangles earcut
// over footprint vertices, which carry ed_discard, NOT normal_ed). Inject FE_INSTANCING so the MSL
// uses a constant top/bottom flag t=1 (a roof cap is always the top) instead of normal_ed.x & 1, and
// drops the absent normal_ed vertex attribute.
constexpr auto shadowDepthShaderPrelude =
#if MLN_USE_FILL_EXTRUSION_INSTANCING
    "#define FE_INSTANCING 1\n"
#else
    "#define FE_INSTANCING 0\n"
#endif
    R"(

enum {
    idShadowDepthDrawableUBO = drawableReservedUBOCount,
    shadowDepthUBOCount
};

struct alignas(16) ShadowDepthDrawableUBO {
    /*  0 */ float4x4 light_matrix;
    /* 64 */ float base_t;
    /* 68 */ float height_t;
    /* 72 */ float u_base;
    /* 76 */ float u_height;
    /* 80 */
};
static_assert(sizeof(ShadowDepthDrawableUBO) == 5 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "ShadowDepthShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    static const std::array<AttributeInfo, 3> attributes; // pos, base, height (roof; no normal_ed)
#else
    static const std::array<AttributeInfo, 4> attributes;
#endif
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static constexpr std::array<TextureInfo, 0> textures{};

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
#if FE_INSTANCING
    // Instanced path: the caster draws the roof cap (footprint verts, no normal_ed). base/height
    // shift down to attribute slots 1/2.
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(1)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(2)]];
#endif
#else
    // Packed wall normal + edge distance; LSB of x is the top/bottom flag (matches FE).
    short4 normal_ed [[attribute(1)]];
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(2)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(3)]];
#endif
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    // The light-clip position, passed through so the fragment can pack ndc.z = z/w —
    // the SAME metric the receiver computes (shadow_pos.z/shadow_pos.w). Packing
    // [[position]].z instead would store window-space depth (remapped through the
    // viewport depth range), which the receiver does not undo → systematic mismatch.
    float4 lightClip;
};

float4 packDepth(float depth) {
    const float maxPackable = 1.0 - 1.0 / 16581375.0;
    depth = clamp(depth, 0.0, maxPackable);
    const float4 bitSh = float4(1.0, 255.0, 65025.0, 16581375.0);
    const float4 mask  = float4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    float4 enc = fract(bitSh * depth);
    enc -= enc.yzww * mask;
    return enc;
}

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const ShadowDepthDrawableUBO& drawable [[buffer(idShadowDepthDrawableUBO)]]) {
    // Match the visible FE / receiver vertex EXACTLY so the caster co-locates with the
    // building you see: same constant in the uniform branch, same per-vertex
    // interpolation factor (NOT a hardcoded 0.0) in the data-driven branch.
#if defined(HAS_UNIFORM_u_base)
    const float base = max(drawable.u_base, 0.0);
#else
    const float base = max(unpack_mix_float(vertx.base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    const float height = max(drawable.u_height, 0.0);
#else
    const float height = max(unpack_mix_float(vertx.height, drawable.height_t), 0.0);
#endif
    // Match the FE vertex: t (top/bottom flag) selects height vs base for z. On the instanced path
    // this caster draws the roof cap, which is always the top, so t is the constant 1.
#if FE_INSTANCING
    const float t = 1.0;
#else
    const float t = float(vertx.normal_ed.x & 1);
#endif
    const float z = (t > 0.0) ? height : base;
    const float4 clip = drawable.light_matrix * float4(float2(vertx.pos), z, 1.0);
    return { .position = clip, .lightClip = clip };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]]) {
    // Pack ndc.z = z/w — the exact metric the receiver compares against. (A seam-probe confirmed
    // [[position]].z == lightClip.z/lightClip.w exactly, so window-space depth and this packed metric
    // agree; the hardware LessEqual test selects the same nearest caster the receiver compares.)
    return half4(packDepth(in.lightClip.z / in.lightClip.w));
}
)";
};

#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced WALL caster. The roof caster (ShadowDepthShader above) casts only the building ROOF on the
// instanced path (sharedTriangles is roof-only there) — so the cast shadow is the footprint projected
// from roof height, DETACHED from the building base. This shader casts the WALLS too: it reconstructs the
// extruded wall quads from the same per-edge OutlineInstance buffer the visible FillExtrusionInstancedShader
// uses (static unit quad × per-instance footprint edge, t = vertx.pos.y selects base vs height), and packs
// light-space depth like the roof caster. Together the roof+wall casters fill the full building volume in
// the shadow map, so ground shadows reattach to the base (matching the non-instanced path, whose
// sharedTriangles already includes walls).
template <>
struct ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "ShadowDepthInstancedShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;         // static quad pos
    static const std::array<AttributeInfo, 5> instanceAttributes; // outline pos + ed_discard, color, base, height
    static constexpr std::array<TextureInfo, 0> textures{};

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(4)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(5)]];
#endif
};

struct OutlineInstance {
    short2 pos;
    ushort2 ed_discard;
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float4 lightClip;
};

float4 packDepth(float depth) {
    const float maxPackable = 1.0 - 1.0 / 16581375.0;
    depth = clamp(depth, 0.0, maxPackable);
    const float4 bitSh = float4(1.0, 255.0, 65025.0, 16581375.0);
    const float4 mask  = float4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    float4 enc = fract(bitSh * depth);
    enc -= enc.yzww * mask;
    return enc;
}

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const ShadowDepthDrawableUBO& drawable [[buffer(idShadowDepthDrawableUBO)]],
                                uint instanceID [[ instance_id ]],
                                device const OutlineInstance* outline [[buffer(shadowDepthUBOCount + 1)]]) {
    // Discarded instance (the ring-closing edge): collapse to a degenerate point so it casts nothing.
    if (outline[instanceID].ed_discard.y) {
        return { .position = float4(0.0), .lightClip = float4(0.0) };
    }
#if defined(HAS_UNIFORM_u_base)
    const float base = max(drawable.u_base, 0.0);
#else
    const float base = max(unpack_mix_float(vertx.base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    const float height = max(drawable.u_height, 0.0);
#else
    const float height = max(unpack_mix_float(vertx.height, drawable.height_t), 0.0);
#endif
    // Static quad: pos.y selects base(0)/height(1); pos.x selects the edge's near/far endpoint.
    const float t = float(vertx.pos.y);
    const float z = (t != 0.0) ? height : base;
    const float4 worldLocal = float4(float2(outline[instanceID + vertx.pos.x].pos), z, 1.0);
    const float4 clip = drawable.light_matrix * worldLocal;
    return { .position = clip, .lightClip = clip };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]]) {
    return half4(packDepth(in.lightClip.z / in.lightClip.w));
}
)";
};
#endif // MLN_USE_FILL_EXTRUSION_INSTANCING

} // namespace shaders
} // namespace mbgl
