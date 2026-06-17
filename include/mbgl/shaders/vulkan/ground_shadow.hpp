#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>

namespace mbgl {
namespace shaders {

// Ground receiver: a flat full-tile quad that darkens where a caster occludes the sun, plus a
// far-cascade radial rim fade and a camera-distance (view-w) depth fade so the shadow edge melts
// into the distance instead of cutting off at the frustum boundary. Mirrors the GLES/Metal
// GroundShadowShader. Vulkan specifics: applySurfaceTransform() on the on-screen position only; the
// light-space positions use the raw light_matrix; no uv.y flip on sampling (GLES convention).

constexpr auto groundShadowShaderPrelude = R"(

#define idGroundShadowDrawableUBO    drawableUBOStartId
#define idGroundShadowPropsUBO       drawableUBOStartId + 1

)";

template <>
struct ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "GroundShadowShader";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 4> textures;

    static constexpr auto prelude = groundShadowShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position;

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idGroundShadowDrawableUBO) uniform GroundShadowDrawableUBO {
    mat4 matrix;
    mat4 light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    int cascade_count;
    float pad0;
    float pad1;
    float pad2;
} drawable;

layout(location = 0) out vec4 v_shadow_pos[4];
layout(location = 4) out float v_view_w;
layout(location = 5) flat out int v_cascade_count;

void main() {
    vec4 worldLocal = vec4(in_position, 0.0, 1.0);
    vec4 clip = drawable.matrix * worldLocal;
    gl_Position = clip;
    applySurfaceTransform();
    v_view_w = clip.w; // perspective view-distance for the near->far depth fade (unaffected by the transform)
    v_cascade_count = drawable.cascade_count;
    v_shadow_pos[0] = drawable.light_matrix[0] * worldLocal;
    v_shadow_pos[1] = drawable.light_matrix[drawable.cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = drawable.light_matrix[drawable.cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = drawable.light_matrix[drawable.cascade_count > 3 ? 3 : 0] * worldLocal;
}

)";

    static constexpr auto fragment = R"(

layout(location = 0) in vec4 v_shadow_pos[4];
layout(location = 4) in float v_view_w;
layout(location = 5) flat in int v_cascade_count;

layout(location = 0) out vec4 out_color;

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idGroundShadowPropsUBO) uniform GroundShadowPropsUBO {
    vec4 shadow_color;
    float shadow_intensity;
    float shadow_texel_size;
    float shadow_bias;
    float shadow_fade_start;
    float depth_fade_start;
    float depth_fade_end;
    float pad0;
    float pad1;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D shadow0_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 1) uniform sampler2D shadow1_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 2) uniform sampler2D shadow2_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 3) uniform sampler2D shadow3_sampler;

float ground_unpackShadowDepth(vec4 rgba) {
    return dot(rgba, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

float ground_depthFade(float view_w, float fade_start, float fade_end) {
    if (fade_end <= 0.0 || fade_end <= fade_start) {
        return 1.0;
    }
    return 1.0 - smoothstep(fade_start, fade_end, view_w);
}

float ground_pcfBilinear(sampler2D tex, vec2 uv, float texel, float current) {
    vec2 tc = uv / texel - 0.5;
    vec2 base = floor(tc);
    vec2 f = tc - base;
    vec2 c00 = (base + 0.5) * texel;
    float s00 = (current <= ground_unpackShadowDepth(texture(tex, c00))) ? 1.0 : 0.0;
    float s10 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(texel, 0.0)))) ? 1.0 : 0.0;
    float s01 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(0.0, texel)))) ? 1.0 : 0.0;
    float s11 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

// Returns lit in [0,1] when this cascade contains the fragment, or -1.0 when it doesn't.
float ground_cascade(sampler2D tex, vec4 sp) {
    vec3 ndc = sp.xyz / sp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5; // no uv.y flip (GLES/Vulkan convention)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    float current = ndc.z - props.shadow_bias;
    float l = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            l += ground_pcfBilinear(
                tex, uv + (vec2(float(dx), float(dy)) - 0.5) * props.shadow_texel_size, props.shadow_texel_size, current);
        }
    }
    return l / 4.0;
}

void main() {
    int lastCascade = max(v_cascade_count - 1, 0);
    // UV-radial (frustum-edge) fade tied to the FAR cascade — the OUTER coverage boundary.
    vec4 farSp = v_shadow_pos[lastCascade];
    vec3 farNdc = farSp.xyz / farSp.w;
    vec2 farUv = farNdc.xy * 0.5 + 0.5; // no uv.y flip
    float r = max(abs(farUv.x - 0.5), abs(farUv.y - 0.5)) * 2.0;
    float uvFade = 1.0 - smoothstep(props.shadow_fade_start, 1.0, r);
    float depthFade = ground_depthFade(v_view_w, props.depth_fade_start, props.depth_fade_end);
    float fade = uvFade * depthFade;

    // Tightest containing cascade wins; static if-chain (no dynamic sampler index in Vulkan GLSL).
    float lit = 1.0;
    float res = -1.0;
    if (v_cascade_count > 0) {
        res = ground_cascade(shadow0_sampler, v_shadow_pos[0]);
        if (res >= 0.0) {
            lit = res;
        } else if (v_cascade_count > 1) {
            res = ground_cascade(shadow1_sampler, v_shadow_pos[1]);
            if (res >= 0.0) {
                lit = res;
            } else if (v_cascade_count > 2) {
                res = ground_cascade(shadow2_sampler, v_shadow_pos[2]);
                if (res >= 0.0) {
                    lit = res;
                } else if (v_cascade_count > 3) {
                    res = ground_cascade(shadow3_sampler, v_shadow_pos[3]);
                    if (res >= 0.0) {
                        lit = res;
                    }
                }
            }
        }
    }

    out_color = vec4(props.shadow_color.rgb, (1.0 - lit) * props.shadow_intensity * fade);
}

)";
};

} // namespace shaders
} // namespace mbgl
