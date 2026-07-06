precision highp float;

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
    float d = dot(rgba, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
    // Unwritten shadow-map texels read all-zero == packed depth 0.0 (the light's near plane). The map
    // is seeded + cleared to white (far, 1.0) so "no caster" reads lit, but a texel the caster pass
    // never populated for the sampled image can still read 0.0 on some backends (observed on
    // Vulkan/Mali: the receiver samples an image whose far-field/edge texels were never covered by the
    // white seed or clear the render/readback sees), and 0.0 compares NEARER than every roof -> the
    // entire far field is wrongly shadowed (the grey-roof "D3" artifact). ShadowFrustum::fit pads the
    // ortho near plane BELOW the tallest caster (zPad), so no real occluder ever encodes depth ~0;
    // treat a ~0 texel as FAR so an unwritten sample never occludes. Behaviour-identical for every real
    // caster (ndc.z >= ~0.02) and the white far seed (1.0); only the all-zero sentinel is remapped.
    return d < (0.5 / 255.0) ? 1.0 : d;
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
