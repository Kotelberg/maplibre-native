#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

// Ground cast-shadow receiver — OpenGL ES port of mtl/ground_shadow.hpp.
// A per-tile ground quad that cascade-selects + bilinear-PCF samples the packed-depth shadow map and
// emits a translucent shadow color, with a far-cascade UV-radial rim fade + a camera-distance
// view-depth fade so the bounded frustum edge reads as a graceful taper at steep pitch.
// GL specifics vs Metal: no uv.y flip (bottom-left FBO origin); static-sampler if-chain for the
// cascade walk (ES 3.0 forbids dynamic sampler-array indexing).
template <>
struct ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "GroundShadowShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;

layout (std140) uniform GroundShadowDrawableUBO {
    highp mat4 u_matrix;
    highp mat4 u_light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    highp int u_cascade_count;
    highp float u_drawable_pad0;
    highp float u_drawable_pad1;
    highp float u_drawable_pad2;
};

out highp vec4 v_shadow_pos[4];
out highp float v_view_w;
flat out int v_cascade_count;

void main() {
    highp vec4 worldLocal = vec4(a_pos, 0.0, 1.0);
    highp vec4 clip = u_matrix * worldLocal;
    gl_Position = clip;
    v_view_w = clip.w; // perspective view-distance for the near->far depth fade
    v_cascade_count = u_cascade_count;
    v_shadow_pos[0] = u_light_matrix[0] * worldLocal;
    v_shadow_pos[1] = u_light_matrix[u_cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = u_light_matrix[u_cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = u_light_matrix[u_cascade_count > 3 ? 3 : 0] * worldLocal;
}
)";
    static constexpr const char* fragment = R"(precision highp float;

in highp vec4 v_shadow_pos[4];
in highp float v_view_w;
flat in int v_cascade_count;

layout (std140) uniform GroundShadowPropsUBO {
    highp vec4 u_shadow_color;
    highp float u_shadow_intensity;
    highp float u_shadow_texel_size;
    highp float u_shadow_bias;
    highp float u_shadow_fade_start;
    highp float u_depth_fade_start;
    highp float u_depth_fade_end;
    highp float u_props_pad0;
    highp float u_props_pad1;
};

uniform highp sampler2D u_shadowmap0;
uniform highp sampler2D u_shadowmap1;
uniform highp sampler2D u_shadowmap2;
uniform highp sampler2D u_shadowmap3;

float ground_unpackShadowDepth(vec4 rgba) {
    return dot(rgba, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

float ground_depthFade(float view_w, float fade_start, float fade_end) {
    if (fade_end <= 0.0 || fade_end <= fade_start) {
        return 1.0;
    }
    return 1.0 - smoothstep(fade_start, fade_end, view_w);
}

float ground_pcfBilinear(highp sampler2D tex, highp vec2 uv, float texel, float current) {
    highp vec2 tc = uv / texel - 0.5;
    highp vec2 base = floor(tc);
    highp vec2 f = tc - base;
    highp vec2 c00 = (base + 0.5) * texel;
    float s00 = (current <= ground_unpackShadowDepth(texture(tex, c00))) ? 1.0 : 0.0;
    float s10 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(texel, 0.0)))) ? 1.0 : 0.0;
    float s01 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(0.0, texel)))) ? 1.0 : 0.0;
    float s11 = (current <= ground_unpackShadowDepth(texture(tex, c00 + vec2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

// Returns lit in [0,1] when this cascade contains the fragment, or -1.0 when it doesn't.
float ground_cascade(highp sampler2D tex, highp vec4 sp) {
    highp vec3 ndc = sp.xyz / sp.w;
    highp vec2 uv = ndc.xy * 0.5 + 0.5; // GL bottom-left origin: no uv.y flip
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    float current = ndc.z - u_shadow_bias;
    float l = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            l += ground_pcfBilinear(
                tex, uv + (vec2(float(dx), float(dy)) - 0.5) * u_shadow_texel_size, u_shadow_texel_size, current);
        }
    }
    return l / 4.0;
}

void main() {
    int lastCascade = max(v_cascade_count - 1, 0);
    // UV-radial (frustum-edge) fade tied to the FAR cascade — the OUTER coverage boundary.
    highp vec4 farSp = v_shadow_pos[lastCascade];
    highp vec3 farNdc = farSp.xyz / farSp.w;
    highp vec2 farUv = farNdc.xy * 0.5 + 0.5; // no uv.y flip on GL
    float r = max(abs(farUv.x - 0.5), abs(farUv.y - 0.5)) * 2.0;
    float uvFade = 1.0 - smoothstep(u_shadow_fade_start, 1.0, r);
    float depthFade = ground_depthFade(v_view_w, u_depth_fade_start, u_depth_fade_end);
    float fade = uvFade * depthFade;

    // Tightest containing cascade wins; static-sampler if-chain (ES 3.0 has no dynamic sampler index).
    float lit = 1.0;
    float res = -1.0;
    if (v_cascade_count > 0) {
        res = ground_cascade(u_shadowmap0, v_shadow_pos[0]);
        if (res >= 0.0) {
            lit = res;
        } else if (v_cascade_count > 1) {
            res = ground_cascade(u_shadowmap1, v_shadow_pos[1]);
            if (res >= 0.0) {
                lit = res;
            } else if (v_cascade_count > 2) {
                res = ground_cascade(u_shadowmap2, v_shadow_pos[2]);
                if (res >= 0.0) {
                    lit = res;
                } else if (v_cascade_count > 3) {
                    res = ground_cascade(u_shadowmap3, v_shadow_pos[3]);
                    if (res >= 0.0) {
                        lit = res;
                    }
                }
            }
        }
    }

    fragColor = vec4(u_shadow_color.rgb, (1.0 - lit) * u_shadow_intensity * fade);
}
)";
};

} // namespace shaders
} // namespace mbgl
