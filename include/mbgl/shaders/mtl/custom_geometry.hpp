#pragma once

#include <mbgl/shaders/custom_geometry_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto customGeometryShaderPrelude = R"(

enum {
    idCustomGeometryDrawableUBO = drawableReservedUBOCount,
    customGeometryUBOCount
};

struct alignas(16) CustomGeometryDrawableUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4 color;
    /*  80 */ float4 highlight;
    /*  96 */ float4 view_axis;
    /* 112 */
};
static_assert(sizeof(CustomGeometryDrawableUBO) == 7 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::CustomGeometryShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "CustomGeometryShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 3> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = customGeometryShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float3 normal [[attribute(2)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
    float3 normal;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const CustomGeometryDrawableUBO& drawable [[buffer(idCustomGeometryDrawableUBO)]]) {

    return {
        .position = drawable.matrix * float4(vertx.position, 1.0),
        .uv = vertx.uv,
        .normal = vertx.normal
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const CustomGeometryDrawableUBO& drawable [[buffer(idCustomGeometryDrawableUBO)]],
                            texture2d<float, access::sample> colorTexture [[texture(0)]]) {
    constexpr sampler sampler2d(coord::normalized, filter::linear);
    float4 color = colorTexture.sample(sampler2d, in.uv) * drawable.color;

    // Optional fresnel rim highlight (model selection). No-op when intensity is 0.
    if (drawable.highlight.a > 0.0) {
        float facing = abs(dot(normalize(in.normal), normalize(drawable.view_axis.xyz)));
        float rim = pow(1.0 - facing, max(drawable.view_axis.w, 0.001));
        color.rgb = mix(color.rgb, drawable.highlight.rgb, clamp(rim * drawable.highlight.a, 0.0, 1.0));
    }

    return half4(color);
}
)";
};

} // namespace shaders
} // namespace mbgl
