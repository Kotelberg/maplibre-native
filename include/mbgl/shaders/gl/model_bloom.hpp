#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

// Composite pass for the model-selection bloom. Samples the offscreen
// silhouette mask of the selected model and emits a soft glow that bleeds
// only OUTWARD from the silhouette (blurred coverage minus the solid mask),
// blended additively over the scene — an atmospheric halo around the
// building rather than a tint on its surface.
template <>
struct ShaderSource<BuiltIn::ModelBloomShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "ModelBloomShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;
out vec2 v_uv;

void main() {
    v_uv = a_pos;
    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static constexpr const char* fragment = R"(in vec2 v_uv;
uniform sampler2D u_image;

layout (std140) uniform ModelBloomDrawableUBO {
    highp vec4 u_color;   // rgb = glow colour, a = intensity
    highp vec2 u_texel;   // 1 / mask size
    highp float u_radius; // blur radius in texels
    highp float u_pad;
};

void main() {
    float mask = texture(u_image, v_uv).a;
    // Two-ring disk blur of the mask coverage.
    float blur = 0.0;
    for (int i = 0; i < 12; i++) {
        float a = (float(i) / 12.0) * 6.2831853;
        vec2 dir = vec2(cos(a), sin(a)) * u_texel * u_radius;
        blur += texture(u_image, v_uv + dir).a * 0.6;
        blur += texture(u_image, v_uv + dir * 0.5).a * 1.0;
    }
    blur /= (12.0 * 1.6);
    // Outward halo only: blurred coverage minus the solid silhouette.
    float halo = clamp(blur - mask, 0.0, 1.0);
    halo = pow(halo, 0.75);
    fragColor = vec4(u_color.rgb, 1.0) * (halo * u_color.a);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(0.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mbgl
