#pragma once

#include <mbgl/shaders/ground_shadow_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto groundShadowShaderPrelude = R"(

enum {
    idGroundShadowDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idGroundShadowPropsUBO = drawableReservedUBOCount,
    groundShadowUBOCount
};

struct alignas(16) GroundShadowDrawableUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4x4 light_matrix;
    /* 128 */
};
static_assert(sizeof(GroundShadowDrawableUBO) == 8 * 16, "wrong size");

struct alignas(16) GroundShadowPropsUBO {
    /*  0 */ float4 shadow_color;
    /* 16 */ float shadow_intensity;
    /* 20 */ float shadow_texel_size;
    /* 24 */ float shadow_bias;
    /* 28 */ float pad1;
    /* 32 */
};
static_assert(sizeof(GroundShadowPropsUBO) == 2 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "GroundShadowShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = groundShadowShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float4 shadow_pos;
};

struct FragmentOutput {
    half4 color [[color(0)]];
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GroundShadowDrawableUBO& drawable [[buffer(idGroundShadowDrawableUBO)]]) {
    const float4 worldLocal = float4(float2(vertx.pos), 0.0, 1.0);
    return {
        .position = drawable.matrix * worldLocal,
        .shadow_pos = drawable.light_matrix * worldLocal,
    };
}

float ground_unpackShadowDepth(float4 rgba) {
    return dot(rgba, float4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

fragment FragmentOutput fragmentMain(FragmentStage in [[stage_in]],
                                     device const GroundShadowPropsUBO& props [[buffer(idGroundShadowPropsUBO)]],
                                     texture2d<float, access::sample> shadowTexture [[texture(0)]]) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    const float3 ndc = in.shadow_pos.xyz / in.shadow_pos.w;
    const float2 uv = ndc.xy * 0.5 + 0.5;
    // VIZ debug (toggle via MLN_SHADOW_INTENSITY > 1.5): paint frustum + caster coverage so the
    // live render reveals the cutoff cause (text logs don't surface in the RN host). blue = UV
    // outside the light frustum (no coverage); red = in frustum + a caster wrote depth (shadowed);
    // green = in frustum, no caster (lit ground).
    if (props.shadow_intensity > 1.5) {
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            return {half4(0.0, 0.0, 1.0, 0.55)};
        }
        const float occlV = ground_unpackShadowDepth(shadowTexture.sample(shadowSampler, uv));
        if (occlV < 0.99) {
            return {half4(1.0, 0.0, 0.0, 0.6)};
        }
        return {half4(0.0, 1.0, 0.0, 0.35)};
    }
    float lit = 1.0;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        const float current = ndc.z - props.shadow_bias;
        lit = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const float occl = ground_unpackShadowDepth(
                    shadowTexture.sample(shadowSampler, uv + float2(dx, dy) * props.shadow_texel_size));
                lit += (current <= occl) ? 1.0 : 0.0;
            }
        }
        lit /= 9.0;
    }

    return {half4(half3(props.shadow_color.rgb), half((1.0 - lit) * props.shadow_intensity))};
}
)";
};

} // namespace shaders
} // namespace mbgl
