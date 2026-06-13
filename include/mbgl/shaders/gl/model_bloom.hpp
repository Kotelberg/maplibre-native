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
    // The offscreen target clears to opaque black (0,0,0,1) and the silhouette
    // is drawn solid white, so coverage lives in the RED channel (alpha is 1
    // everywhere because of the clear — see RenderTarget::render).
    float mask = texture(u_image, v_uv).r;
    // Wide three-ring disk blur of the silhouette coverage for a soft,
    // atmospheric falloff.
    float blur = 0.0;
    for (int i = 0; i < 16; i++) {
        float ang = (float(i) / 16.0) * 6.2831853;
        vec2 dir = vec2(cos(ang), sin(ang)) * u_texel * u_radius;
        blur += texture(u_image, v_uv + dir).r;
        blur += texture(u_image, v_uv + dir * 0.66).r;
        blur += texture(u_image, v_uv + dir * 0.33).r;
    }
    blur /= 48.0;
    // Outer glow that HUGS the silhouette: blurred coverage gated to outside
    // the geometry. Multiplicative gating (vs. blur - mask) leaves no dead-band
    // between the model edge and the glow, and never tints the model itself.
    float halo = clamp(blur * (1.0 - mask) * 1.9, 0.0, 1.0);
    halo = pow(halo, 0.9);
    // Premultiplied-alpha output: the halo TINTS the scene toward the glow
    // colour (visible on a bright basemap) instead of merely brightening it.
    float a = halo * u_color.a;
    fragColor = vec4(u_color.rgb * a, a);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(0.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mbgl
