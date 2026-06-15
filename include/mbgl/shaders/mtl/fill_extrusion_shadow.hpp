#pragma once

#include <mbgl/shaders/fill_extrusion_shadow_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

// Shadow-receiving fill-extrusion shader. Mirrors the non-pattern FillExtrusionShader vertex
// lighting verbatim, adds a light-space position varying, and a fragment that PCF-samples the
// shadow map and attenuates the lit color. Non-consolidated single drawable UBO.
constexpr auto fillExtrusionShadowShaderPrelude = R"(

enum {
    idFillExtrusionShadowDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idFillExtrusionShadowPropsUBO = drawableReservedUBOCount,
    fillExtrusionShadowUBOCount
};

struct alignas(16) FillExtrusionShadowDrawableUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4x4 light_matrix;
    /* 128 */ float base_t;
    /* 132 */ float height_t;
    /* 136 */ float color_t;
    /* 140 */ float pad1;
    /* 144 */
};
static_assert(sizeof(FillExtrusionShadowDrawableUBO) == 9 * 16, "wrong size");

struct alignas(16) FillExtrusionShadowPropsUBO {
    /*  0 */ float4 color;
    /* 16 */ float4 light_color_pad;
    /* 32 */ float4 light_position_base;
    /* 48 */ float height;
    /* 52 */ float light_intensity;
    /* 56 */ float vertical_gradient;
    /* 60 */ float opacity;
    /* 64 */ float shadow_intensity;
    /* 68 */ float shadow_texel_size;
    /* 72 */ float shadow_bias;
    /* 76 */ float shadow_slope_bias; // extra bias scaled by (1 - n·L); kills self-shadowing
    /* 80 */
};
static_assert(sizeof(FillExtrusionShadowPropsUBO) == 5 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "FillExtrusionShadowShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 5> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = fillExtrusionShadowShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
    short4 normal_ed [[attribute(1)]];

#if !defined(HAS_UNIFORM_u_color)
    float4 color [[attribute(2)]];
#endif
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(3)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(4)]];
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    half4 color;
    float4 shadow_pos;
    // (1 - n·L): 0 on sun-facing faces, →1 on faces turned away from the sun. Scales the
    // depth bias so a building never shadows its OWN away-faces (the directional lighting
    // already darkens those); only a neighbour's cast shadow falling across it darkens it.
    float slope;
};

struct FragmentOutput {
    half4 color [[color(0)]];
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const FillExtrusionShadowDrawableUBO& drawable [[buffer(idFillExtrusionShadowDrawableUBO)]],
                                device const FillExtrusionShadowPropsUBO& props [[buffer(idFillExtrusionShadowPropsUBO)]]) {

#if defined(HAS_UNIFORM_u_base)
    const auto base   = props.light_position_base.w;
#else
    const auto base   = max(unpack_mix_float(vertx.base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    const auto height = props.height;
#else
    const auto height = max(unpack_mix_float(vertx.height, drawable.height_t), 0.0);
#endif

    const float t = float(vertx.normal_ed.x & 1);
    const float3 normal = float3(vertx.normal_ed.xyz) / 16384.0;
    const float z = (t > 0.0) ? height : base;
    const float4 worldLocal = float4(float2(vertx.pos), z, 1);
    const float4 position = drawable.matrix * worldLocal;

#if defined(HAS_UNIFORM_u_color)
    auto color = props.color;
#else
    auto color = unpack_mix_color(vertx.color, drawable.color_t);
#endif

    const float luminance = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    float4 vcolor = float4(0.0, 0.0, 0.0, 1.0);
    color += min(float4(0.03, 0.03, 0.03, 1.0), float4(1.0));

    const float directionalFraction = clamp(dot(normal, props.light_position_base.xyz), 0.0, 1.0);
    const float minDirectional = 1.0 - props.light_intensity;
    const float maxDirectional = max(1.0 - luminance + props.light_intensity, 1.0);
    float directional = mix(minDirectional, maxDirectional, directionalFraction);

    if (normal.y != 0.0) {
        const float fMin = mix(0.7, 0.98, 1.0 - props.light_intensity);
        const float factor = clamp((t + base) * pow(height / 150.0, 0.5), fMin, 1.0);
        directional *= (1.0 - props.vertical_gradient) + (props.vertical_gradient * factor);
    }

    const float3 light_color = props.light_color_pad.rgb;
    const float3 minLight = mix(0.0, 0.3, 1.0 - light_color.rgb);
    vcolor += float4(clamp(color.rgb * directional * light_color.rgb, minLight, 1.0), 0.0);

    return {
        .position = position,
        .color    = half4(vcolor * props.opacity),
        .shadow_pos = drawable.light_matrix * worldLocal,
        .slope = 1.0 - directionalFraction,
    };
}

float fe_unpackShadowDepth(float4 rgba) {
    return dot(rgba, float4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

fragment FragmentOutput fragmentMain(FragmentStage in [[stage_in]],
                                     device const FillExtrusionShadowPropsUBO& props [[buffer(idFillExtrusionShadowPropsUBO)]],
                                     texture2d<float, access::sample> shadowTexture [[texture(0)]]) {
    half4 color = in.color;
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    const float3 ndc = in.shadow_pos.xyz / in.shadow_pos.w;
    const float2 uv = ndc.xy * 0.5 + 0.5;
    // Slope-scaled bias: away-from-sun faces get a large bias so they never self-shadow
    // (their shading is the directional light's job); sun-facing faces keep the small base
    // bias so a neighbour's cast shadow still lands on them. Result: building shadows read as
    // "the ground shadow extended up where another building occludes it", not per-face grey.
    const float current = ndc.z - (props.shadow_bias + in.slope * props.shadow_slope_bias);
    // Inside the light frustum in all three axes (see ground_shadow.hpp): the depth-range guard
    // prevents fragments beyond the far/near plane (ndc.z outside [0,1]) from reading phantom shadow.
    float lit = 1.0;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && ndc.z >= 0.0 && ndc.z <= 1.0) {
        lit = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const float occl = fe_unpackShadowDepth(
                    shadowTexture.sample(shadowSampler, uv + float2(dx, dy) * props.shadow_texel_size));
                lit += (current <= occl) ? 1.0 : 0.0;
            }
        }
        lit /= 9.0;
    }
    color.rgb *= half(1.0 - (1.0 - lit) * props.shadow_intensity);
    return { color };
}
)";
};

} // namespace shaders
} // namespace mbgl
