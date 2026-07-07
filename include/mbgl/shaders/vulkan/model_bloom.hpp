#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto modelBloomShaderPrelude = R"(#define idModelBloomDrawableUBO  drawableUBOStartId)";

// Composite pass for the model-selection bloom (Vulkan). Samples the offscreen
// silhouette mask of the selected model and emits a soft glow that hugs the
// silhouette and bleeds only OUTWARD, blended over the scene as a premultiplied
// tint — an atmospheric halo around the building, not a tint on its surface.
// Ported from the GL/Metal fork shader into the Vulkan GLSL dialect.
template <>
struct ShaderSource<BuiltIn::ModelBloomShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "ModelBloomShader";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = modelBloomShaderPrelude;
    static constexpr auto vertex = R"(
layout(location = 0) in vec2 in_position;

layout(location = 0) out vec2 frag_uv;

void main() {
    frag_uv = in_position;

    gl_Position = vec4(in_position * 2.0 - 1.0, 0.0, 1.0);
    applySurfaceTransform();
}
)";

    static constexpr auto fragment = R"(
layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idModelBloomDrawableUBO) uniform ModelBloomDrawableUBO {
    vec4 color;   // rgb = glow colour, a = intensity
    vec2 texel;   // 1 / mask size
    float radius; // blur radius in texels
    float pad;
} drawable;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D image_sampler;

void main() {

#if defined(OVERDRAW_INSPECTOR)
    out_color = vec4(0.0);
    return;
#endif

    // The offscreen target clears to opaque black (0,0,0,1) and the silhouette
    // is drawn solid white, so coverage lives in the RED channel.
    float mask = texture(image_sampler, frag_uv).r;
    // Wide three-ring disk blur of the silhouette coverage for a soft,
    // atmospheric falloff.
    float blur = 0.0;
    for (int i = 0; i < 16; i++) {
        float ang = (float(i) / 16.0) * 6.2831853;
        vec2 dir = vec2(cos(ang), sin(ang)) * drawable.texel * drawable.radius;
        blur += texture(image_sampler, frag_uv + dir).r;
        blur += texture(image_sampler, frag_uv + dir * 0.66).r;
        blur += texture(image_sampler, frag_uv + dir * 0.33).r;
    }
    blur /= 48.0;
    // Outer glow that HUGS the silhouette: blurred coverage gated to outside
    // the geometry. Multiplicative gating leaves no dead-band at the edge.
    float halo = clamp(blur * (1.0 - mask) * 1.9, 0.0, 1.0);
    halo = pow(halo, 0.9);
    // Premultiplied-alpha output: tints the scene toward the glow colour.
    float a = halo * drawable.color.a;
    out_color = vec4(drawable.color.rgb * a, a);
}
)";
};

} // namespace shaders
} // namespace mbgl
