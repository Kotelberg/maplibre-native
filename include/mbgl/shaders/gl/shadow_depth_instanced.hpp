#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

// Instanced wall shadow-caster — OpenGL registration placeholder.
//
// The instanced fill-extrusion path (and therefore the dedicated instanced WALL caster) is Metal/
// Vulkan only: MLN_USE_FILL_EXTRUSION_INSTANCING is 0 on OpenGL, so on the GL backend the walls are
// cast by the non-instanced ShadowDepthShader (gl/shadow_depth.hpp) together with the roof — this
// ShadowDepthInstancedShader is NEVER registered (gl/renderer_backend.cpp + gl/shader_info.cpp don't
// reference it) nor executed on GL. This specialization exists only so the generated shader manifest,
// which enumerates every BuiltIn for all backends, compiles. If a GL instancing path is ever added,
// replace this with a real port of mtl/shadow_depth.hpp's ShadowDepthInstancedShader (per-edge
// OutlineInstance geometry → light-space packed depth). It is kept a valid, self-contained shader so
// a stray instantiation still compiles.
template <>
struct ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "ShadowDepthInstancedShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;

layout (std140) uniform ShadowDepthDrawableUBO {
    highp mat4 u_light_matrix;
    highp float u_base_t;
    highp float u_height_t;
    highp float u_base;
    highp float u_height;
};

out highp float v_depth01;

void main() {
    highp vec4 clip = u_light_matrix * vec4(a_pos, u_height, 1.0);
    v_depth01 = clip.z / clip.w;
    clip.z = 2.0 * clip.z - clip.w; // [0,1] -> GL [-1,1] NDC (matches gl/shadow_depth.hpp)
    gl_Position = clip;
}
)";
    static constexpr const char* fragment = R"(precision highp float;

in highp float v_depth01;

vec4 packDepth(float depth) {
    const float maxPackable = 1.0 - 1.0 / 16581375.0;
    depth = clamp(depth, 0.0, maxPackable);
    const vec4 bitSh = vec4(1.0, 255.0, 65025.0, 16581375.0);
    const vec4 mask  = vec4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    vec4 enc = fract(bitSh * depth);
    enc -= enc.yzww * mask;
    return enc;
}

void main() {
    fragColor = packDepth(v_depth01);
}
)";
};

} // namespace shaders
} // namespace mbgl
