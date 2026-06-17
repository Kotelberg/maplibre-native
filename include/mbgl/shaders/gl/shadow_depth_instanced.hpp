#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

// Instanced wall shadow-caster (OpenGL, fork). Edge-indexed port of mtl/shadow_depth.hpp's
// ShadowDepthInstancedShader: reconstructs each wall quad from the static unit quad + this edge's two
// endpoints (per-instance a_pos0/a_pos1) and packs its light-space depth. Used on the GL instanced
// path (MLN_GL_FE_INSTANCING) so the building WALLS enter the shadow map — the gated bucket emits
// roof-only triangles, so the non-instanced ShadowDepthShader caster covers only the roof; without
// this the ground shadow would detach from the building base. Hand-written (not generated); keep the
// #ifndef HAS_UNIFORM_* data-driven blocks in sync with gl/shadow_depth.hpp. NEVER run the shader
// generator over this file (it would clobber it — see plan deviations).
template <>
struct ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "ShadowDepthInstancedShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;    // static unit quad: x=endpoint, y=base/top
layout (location = 1) in vec2 a_pos0;   // instance: edge start
layout (location = 2) in vec2 a_pos1;   // instance: edge end

layout (std140) uniform ShadowDepthDrawableUBO {
    highp mat4 u_light_matrix;
    highp float u_base_t;
    highp float u_height_t;
    highp float u_base;
    highp float u_height;
};

out highp float v_depth01;

#ifndef HAS_UNIFORM_u_base
layout (location = 3) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 4) in highp vec2 a_height;
#endif

void main() {
    // Match the visible instanced FE wall vertex EXACTLY (same interpolation, same endpoint/base-top
    // selection) so the caster co-locates with the building you see.
    #ifndef HAS_UNIFORM_u_base
    highp float base = unpack_mix_vec2(a_base, u_base_t);
    #else
    highp float base = u_base;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = unpack_mix_vec2(a_height, u_height_t);
    #else
    highp float height = u_height;
    #endif
    base = max(0.0, base);
    height = max(0.0, height);

    vec2 footprint = (a_pos.x < 0.5) ? a_pos0 : a_pos1;
    highp vec4 clip = u_light_matrix * vec4(footprint, a_pos.y > 0.5 ? height : base, 1.0);
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
