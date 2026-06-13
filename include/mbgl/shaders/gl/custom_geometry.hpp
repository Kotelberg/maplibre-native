// Generated code, do not modify this file!
#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::CustomGeometryShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "CustomGeometryShader";
    static constexpr const char* vertex = R"(layout (std140) uniform CustomGeometryDrawableUBO {
    mat4 u_matrix;
    vec4 u_color;
    vec4 u_highlight;
    vec4 u_view_axis;
};

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec3 a_normal;

out vec2 frag_uv;
out vec3 frag_normal;

void main() {
    frag_uv = a_uv;
    frag_normal = a_normal;
    gl_Position = u_matrix * vec4(a_pos, 1.0);
}
)";
    static constexpr const char* fragment = R"(layout (std140) uniform CustomGeometryDrawableUBO {
    mat4 u_matrix;
    vec4 u_color;
    vec4 u_highlight;
    vec4 u_view_axis;
};

in vec2 frag_uv;
in vec3 frag_normal;
uniform sampler2D u_image;

void main() {
    vec4 color = texture(u_image, frag_uv) * u_color;
    // Optional fresnel rim highlight (model selection). When intensity is 0
    // this branch is a no-op and the colour is unchanged.
    if (u_highlight.a > 0.0) {
        float facing = abs(dot(normalize(frag_normal), normalize(u_view_axis.xyz)));
        float rim = pow(1.0 - facing, max(u_view_axis.w, 0.001));
        color.rgb = mix(color.rgb, u_highlight.rgb, clamp(rim * u_highlight.a, 0.0, 1.0));
    }
    fragColor = color;
}
)";
};

} // namespace shaders
} // namespace mbgl
