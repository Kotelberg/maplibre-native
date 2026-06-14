#pragma once

#include <mbgl/shaders/shadow_depth_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto shadowDepthShaderPrelude = R"(

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

    static const std::array<AttributeInfo, 4> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static constexpr std::array<TextureInfo, 0> textures{};

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
    // Packed wall normal + edge distance; LSB of x is the top/bottom flag (matches FE).
    short4 normal_ed [[attribute(1)]];
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(2)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(3)]];
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
    // Match the FE vertex: t (top/bottom flag) selects height vs base for z.
    const float t = float(vertx.normal_ed.x & 1);
    const float z = (t > 0.0) ? height : base;
    const float4 clip = drawable.light_matrix * float4(float2(vertx.pos), z, 1.0);
    return { .position = clip, .lightClip = clip };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]]) {
    // Pack ndc.z = z/w — the exact metric the receiver compares against. (Using
    // [[position]].z would be window-space depth, viewport-remapped, and mismatch.)
    return half4(packDepth(in.lightClip.z / in.lightClip.w));
}
)";
};

} // namespace shaders
} // namespace mbgl
