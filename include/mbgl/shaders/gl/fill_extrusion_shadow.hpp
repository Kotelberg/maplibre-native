#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

// Shadow-receiving fill-extrusion shader — OpenGL ES port of mtl/fill_extrusion_shadow.hpp.
// Mirrors the non-pattern FillExtrusionShader vertex lighting, adds per-cascade light-space position
// varyings, and a fragment that cascade-selects + bilinear-PCF samples the packed-depth shadow map,
// suppresses the (aliased) cast shadow on near-vertical walls, and attenuates the lit color.
// GL specifics vs Metal: array varyings are legal (no flattening); the offscreen FBO has a
// BOTTOM-left origin so there is NO uv.y flip; ES 3.0 forbids dynamic sampler-array indexing, so the
// "tightest containing cascade wins" loop is unrolled into a static-sampler if-chain.
template <>
struct ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "FillExtrusionShadowShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec4 a_normal_ed;

layout (std140) uniform FillExtrusionShadowDrawableUBO {
    highp mat4 u_matrix;
    highp mat4 u_light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    highp float u_base_t;
    highp float u_height_t;
    highp float u_color_t;
    highp int u_cascade_count;
};

layout (std140) uniform FillExtrusionShadowPropsUBO {
    highp vec4 u_color;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_base; // xyz = light dir, w = base (uniform path)
    highp float u_height;
    highp float u_light_intensity;
    highp float u_vertical_gradient;
    highp float u_opacity;
    highp float u_shadow_intensity;
    highp float u_shadow_texel_size;
    highp float u_shadow_bias;
    highp float u_shadow_slope_bias;
};

out highp vec4 v_color;
out highp vec4 v_shadow_pos[4];
out highp float v_slope;
out highp float v_wallness;
flat out int v_cascade_count;

#ifndef HAS_UNIFORM_u_color
layout (location = 4) in highp vec4 a_color;
#endif
#ifndef HAS_UNIFORM_u_base
layout (location = 2) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 3) in highp vec2 a_height;
#endif

void main() {
    #ifndef HAS_UNIFORM_u_base
    highp float base = max(unpack_mix_vec2(a_base, u_base_t), 0.0);
    #else
    highp float base = u_light_position_base.w;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = max(unpack_mix_vec2(a_height, u_height_t), 0.0);
    #else
    highp float height = u_height;
    #endif

    highp float t = mod(a_normal_ed.x, 2.0);
    highp vec3 normal = a_normal_ed.xyz / 16384.0;
    highp float z = t > 0.0 ? height : base;
    highp vec4 worldLocal = vec4(a_pos, z, 1.0);
    gl_Position = u_matrix * worldLocal;

    #ifndef HAS_UNIFORM_u_color
    highp vec4 color = unpack_mix_color(a_color, u_color_t);
    #else
    highp vec4 color = u_color;
    #endif

    highp float luminance = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    v_color = vec4(0.0, 0.0, 0.0, 1.0);
    color += min(vec4(0.03, 0.03, 0.03, 1.0), vec4(1.0));

    highp float directionalFraction = clamp(dot(normal, u_light_position_base.xyz), 0.0, 1.0);
    highp float minDirectional = 1.0 - u_light_intensity;
    highp float maxDirectional = max(1.0 - luminance + u_light_intensity, 1.0);
    highp float directional = mix(minDirectional, maxDirectional, directionalFraction);

    if (normal.y != 0.0) {
        highp float fMin = mix(0.7, 0.98, 1.0 - u_light_intensity);
        highp float factor = clamp((t + base) * pow(height / 150.0, 0.5), fMin, 1.0);
        directional *= (1.0 - u_vertical_gradient) + (u_vertical_gradient * factor);
    }

    highp vec3 light_color = u_light_color_pad.rgb;
    highp vec3 minLight = mix(vec3(0.0), vec3(0.3), 1.0 - light_color);
    v_color += vec4(clamp(color.rgb * directional * light_color, minLight, vec3(1.0)), 0.0);
    v_color *= u_opacity;

    // (1 - n·L): 0 on sun-facing faces, ->1 on faces turned away; scales the depth bias so a building
    // never shadows its OWN away-faces.
    v_slope = 1.0 - directionalFraction;
    // 1.0 on vertical walls (normal.z~0), 0.0 on roofs (normal.z~+-1).
    v_wallness = 1.0 - abs(normal.z);
    v_cascade_count = u_cascade_count;

    // Project into every cascade's light clip (near->far). Unused slots replicate cascade 0.
    v_shadow_pos[0] = u_light_matrix[0] * worldLocal;
    v_shadow_pos[1] = u_light_matrix[u_cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = u_light_matrix[u_cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = u_light_matrix[u_cascade_count > 3 ? 3 : 0] * worldLocal;
}
)";
    static constexpr const char* fragment = R"(precision highp float;

in highp vec4 v_color;
in highp vec4 v_shadow_pos[4];
in highp float v_slope;
in highp float v_wallness;
flat in int v_cascade_count;

layout (std140) uniform FillExtrusionShadowPropsUBO {
    highp vec4 u_color;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_base;
    highp float u_height;
    highp float u_light_intensity;
    highp float u_vertical_gradient;
    highp float u_opacity;
    highp float u_shadow_intensity;
    highp float u_shadow_texel_size;
    highp float u_shadow_bias;
    highp float u_shadow_slope_bias;
};

uniform highp sampler2D u_shadowmap0;
uniform highp sampler2D u_shadowmap1;
uniform highp sampler2D u_shadowmap2;
uniform highp sampler2D u_shadowmap3;

float fe_unpackShadowDepth(vec4 rgba) {
    return dot(rgba, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

// Bilinear percentage-closer filter: compare the 4 texels around `uv`, then bilinearly blend the
// 0/1 results. The map stores RGBA8-PACKED depth, so we must compare FIRST, then blend.
float fe_pcfBilinear(highp sampler2D tex, highp vec2 uv, float texel, float current) {
    highp vec2 tc = uv / texel - 0.5;
    highp vec2 base = floor(tc);
    highp vec2 f = tc - base;
    highp vec2 c00 = (base + 0.5) * texel;
    float s00 = (current <= fe_unpackShadowDepth(texture(tex, c00))) ? 1.0 : 0.0;
    float s10 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(texel, 0.0)))) ? 1.0 : 0.0;
    float s01 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(0.0, texel)))) ? 1.0 : 0.0;
    float s11 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

// Returns lit in [0,1] when this cascade contains the fragment, or -1.0 when it doesn't (so the
// caller falls through to the next, wider cascade).
float fe_cascade(highp sampler2D tex, highp vec4 sp, int cIdx) {
    highp vec3 ndc = sp.xyz / sp.w;
    highp vec2 uv = ndc.xy * 0.5 + 0.5; // GL bottom-left origin: no uv.y flip (unlike Metal)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    // PER-CASCADE bias scale: the far cascade covers a huge area (low texel density), so a flat roof
    // still has a large light-space depth gradient across a texel under a grazing sun → self-shadow
    // acne on roofs. Scale the bias up on every cascade (near tight-frustum self-shadow + far low-
    // density grazing acne). Building receiver only (the ground receiver has its own bias); walls are
    // separately suppressed, so extra bias here can't leak onto walls.
    float biasScale = (cIdx < v_cascade_count - 1) ? 8.0 : 4.0;
    float current = ndc.z - (u_shadow_bias + v_slope * u_shadow_slope_bias) * biasScale;
    float l = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            l += fe_pcfBilinear(
                tex, uv + (vec2(float(dx), float(dy)) - 0.5) * u_shadow_texel_size, u_shadow_texel_size, current);
        }
    }
    return l / 4.0;
}

void main() {
    highp vec4 color = v_color;

    // CASCADED SHADOW MAPS: tightest containing cascade wins (hard transition). ES 3.0 cannot index a
    // sampler array by a loop variable, so the near->far walk is an explicit static-sampler if-chain.
    float lit = 1.0;
    float r = -1.0;
    if (v_cascade_count > 0) {
        r = fe_cascade(u_shadowmap0, v_shadow_pos[0], 0);
        if (r >= 0.0) {
            lit = r;
        } else if (v_cascade_count > 1) {
            r = fe_cascade(u_shadowmap1, v_shadow_pos[1], 1);
            if (r >= 0.0) {
                lit = r;
            } else if (v_cascade_count > 2) {
                r = fe_cascade(u_shadowmap2, v_shadow_pos[2], 2);
                if (r >= 0.0) {
                    lit = r;
                } else if (v_cascade_count > 3) {
                    r = fe_cascade(u_shadowmap3, v_shadow_pos[3], 3);
                    if (r >= 0.0) {
                        lit = r;
                    }
                }
            }
        }
    }

    // Suppress the (projective-aliased) cast shadow on near-vertical walls; keep it on roofs.
    lit = mix(lit, 1.0, smoothstep(0.4, 0.85, v_wallness));
    color.rgb *= (1.0 - (1.0 - lit) * u_shadow_intensity);
    fragColor = color;

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mbgl
