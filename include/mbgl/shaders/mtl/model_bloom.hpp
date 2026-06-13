#pragma once

#include <mbgl/shaders/model_bloom_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto modelBloomShaderPrelude = R"(

enum {
    idModelBloomDrawableUBO = drawableReservedUBOCount,
    modelBloomUBOCount
};

struct alignas(16) ModelBloomDrawableUBO {
    /*  0 */ float4 color;   // rgb = glow colour, a = intensity
    /* 16 */ float2 texel;   // 1 / mask size
    /* 24 */ float radius;   // blur radius in texels
    /* 28 */ float pad;
    /* 32 */
};
static_assert(sizeof(ModelBloomDrawableUBO) == 2 * 16, "wrong size");

)";

// Composite pass for the model-selection bloom (Metal). Samples the offscreen
// silhouette mask of the selected model and emits a soft glow that hugs the
// silhouette and bleeds only OUTWARD, blended over the scene as a premultiplied
// tint — an atmospheric halo around the building, not a tint on its surface.
template <>
struct ShaderSource<BuiltIn::ModelBloomShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "ModelBloomShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = modelBloomShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    float2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]]) {
    return {
        .position = float4(vertx.pos * 2.0 - 1.0, 0.0, 1.0),
        // Metal offscreen textures have a top-left origin (opposite the NDC
        // y-up of the silhouette render), so flip v to align the mask with the
        // model on screen. (GL's bottom-left origin needs no flip.)
        .uv = float2(vertx.pos.x, 1.0 - vertx.pos.y)
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const ModelBloomDrawableUBO& drawable [[buffer(idModelBloomDrawableUBO)]],
                            texture2d<float, access::sample> image [[texture(0)]]) {
    constexpr sampler im_sampler(coord::normalized, filter::linear, address::clamp_to_edge);

    // The offscreen target clears to opaque black (0,0,0,1) and the silhouette
    // is drawn solid white, so coverage lives in the RED channel.
    float mask = image.sample(im_sampler, in.uv).r;
    float blur = 0.0;
    for (int i = 0; i < 16; i++) {
        float ang = (float(i) / 16.0) * 6.2831853;
        float2 dir = float2(cos(ang), sin(ang)) * drawable.texel * drawable.radius;
        blur += image.sample(im_sampler, in.uv + dir).r;
        blur += image.sample(im_sampler, in.uv + dir * 0.66).r;
        blur += image.sample(im_sampler, in.uv + dir * 0.33).r;
    }
    blur /= 48.0;
    // Outer glow that HUGS the silhouette: blurred coverage gated to outside
    // the geometry. Multiplicative gating leaves no dead-band at the edge.
    float halo = clamp(blur * (1.0 - mask) * 1.9, 0.0, 1.0);
    halo = pow(halo, 0.9);
    // Premultiplied-alpha output: tints the scene toward the glow colour.
    float a = halo * drawable.color.a;

#if defined(OVERDRAW_INSPECTOR)
    return half4(0.0);
#endif

    return half4(half3(drawable.color.rgb) * half(a), half(a));
}
)";
};

} // namespace shaders
} // namespace mbgl
