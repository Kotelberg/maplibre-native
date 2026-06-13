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
    /* 64 */
};
static_assert(sizeof(ShadowDepthDrawableUBO) == 4 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "ShadowDepthShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 3> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static constexpr std::array<TextureInfo, 0> textures{};

    static constexpr auto prelude = shadowDepthShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos    [[attribute(0)]];
    float  base   [[attribute(1)]];
    float  height [[attribute(2)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
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
    // Extrude to the top of the building (matches the non-pattern FE vertex z = max(height, base)).
    const float z = max(vertx.height, vertx.base);
    return { .position = drawable.light_matrix * float4(float2(vertx.pos), z, 1.0) };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]]) {
    // in.position.z is the post-projection light-clip depth in [0,1] (Metal NDC).
    return half4(packDepth(in.position.z));
}
)";
};

} // namespace shaders
} // namespace mbgl
