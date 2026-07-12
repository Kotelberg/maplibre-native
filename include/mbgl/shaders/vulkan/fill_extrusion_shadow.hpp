#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>
#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

// Building receiver: the non-pattern fill-extrusion draw, swapped to this shader when shadows are
// on. Re-projects each fragment into every cascade's light space and darkens it where a nearer
// caster occludes the sun (PCF-filtered). Mirrors the GLES/Metal FillExtrusionShadowShader.
// Vulkan specifics: the on-screen position gets applySurfaceTransform() (Vulkan y-flip + Android
// surface rotation); the per-cascade light-space positions do NOT (they use the raw light_matrix,
// matching the un-transformed caster). No uv.y flip on sampling (GLES convention — the offscreen
// rasterize/sample conventions cancel). Reuses the FE vertex-attribute ids (pos/normal_ed +
// data-driven color/base/height) so the FE bucket binders feed this shader. The 4 cascade maps are
// 4 discrete samplers (Vulkan has no array-of-textures binding here) walked by a static if-chain.

constexpr auto fillExtrusionShadowShaderPrelude =
#if MLN_USE_FILL_EXTRUSION_INSTANCING
    "#define FE_INSTANCING 1\n"
#else
    "#define FE_INSTANCING 0\n"
#endif
    R"(

#define idFillExtrusionShadowDrawableUBO    drawableUBOStartId
// Receiver PROPS is a LAYER-set UBO (one slot above the plain FillExtrusionPropsUBO the walls bind, so
// the two never alias in the shared layer descriptor set). Uploaded once per layer per frame, it is
// present for every receiver roof drawable — including one the per-drawable tweaker visitor skips on a
// churned surface (a stale tweaker ref / missing binders), which a per-drawable descriptor would leave
// resolved to the dummy zero buffer (opacity 0 -> transparent roof). The matrix stays per-drawable.
#define idFillExtrusionShadowPropsUBO       layerUBOStartId + 1

)";

template <>
struct ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "FillExtrusionShadowShader";

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    static const std::array<AttributeInfo, 4> attributes; // pos, color, base, height (roof; no normal_ed)
#else
    static const std::array<AttributeInfo, 5> attributes;
#endif
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 4> textures;

    static constexpr auto prelude = fillExtrusionShadowShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position;
#if !FE_INSTANCING
layout(location = 1) in ivec4 in_normal_ed;
#endif

// Instanced roof receiver: no normal_ed; color/base/height shift down one slot to 1/2/3.
#if FE_INSTANCING
#if !defined(HAS_UNIFORM_u_color)
layout(location = 1) in vec4 in_color;
#endif
#if !defined(HAS_UNIFORM_u_base)
layout(location = 2) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 3) in vec2 in_height;
#endif
#else
#if !defined(HAS_UNIFORM_u_color)
layout(location = 2) in vec4 in_color;
#endif
#if !defined(HAS_UNIFORM_u_base)
layout(location = 3) in vec2 in_base;
#endif
#if !defined(HAS_UNIFORM_u_height)
layout(location = 4) in vec2 in_height;
#endif
#endif

layout(set = DRAWABLE_UBO_SET_INDEX, binding = idFillExtrusionShadowDrawableUBO) uniform FillExtrusionShadowDrawableUBO {
    mat4 matrix;
    mat4 light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    float base_t;
    float height_t;
    float color_t;
    int cascade_count;
} drawable;

layout(set = LAYER_SET_INDEX, binding = idFillExtrusionShadowPropsUBO) uniform FillExtrusionShadowPropsUBO {
    vec4 color;
    vec4 light_color_pad;
    vec4 light_position_base; // xyz = light dir, w = base (uniform path)
    float height;
    float light_intensity;
    float vertical_gradient;
    float opacity;
    float shadow_intensity;
    float shadow_texel_size;
    float shadow_bias;
    float shadow_slope_bias;
} props;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_shadow_pos[4];
layout(location = 5) out float v_slope;
layout(location = 6) out float v_wallness;
layout(location = 7) flat out int v_cascade_count;

void main() {
#if defined(HAS_UNIFORM_u_base)
    float base = props.light_position_base.w;
#else
    float base = max(unpack_mix_float(in_base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    float height = props.height;
#else
    float height = max(unpack_mix_float(in_height, drawable.height_t), 0.0);
#endif

    // Instanced path: this receiver draws the flat roof cap (always the top), so the normal is the
    // up vector and t is 1. wallness = 1 - abs(normal.z) = 0 → roofs keep their crisp cast shadow.
#if FE_INSTANCING
    float t = 1.0;
    vec3 normal = vec3(0.0, 0.0, 1.0);
#else
    float t = float(in_normal_ed.x & 1);
    vec3 normal = vec3(in_normal_ed.xyz) / 16384.0;
#endif
    float z = t > 0.0 ? height : base;
    vec4 worldLocal = vec4(in_position, z, 1.0);
    gl_Position = drawable.matrix * worldLocal;
    applySurfaceTransform();

#if defined(HAS_UNIFORM_u_color)
    vec4 color = props.color;
#else
    vec4 color = unpack_mix_color(in_color, drawable.color_t);
#endif

    float luminance = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    v_color = vec4(0.0, 0.0, 0.0, 1.0);
    color += min(vec4(0.03, 0.03, 0.03, 1.0), vec4(1.0));

    float directionalFraction = clamp(dot(normal, props.light_position_base.xyz), 0.0, 1.0);
    float minDirectional = 1.0 - props.light_intensity;
    float maxDirectional = max(1.0 - luminance + props.light_intensity, 1.0);
    float directional = mix(minDirectional, maxDirectional, directionalFraction);

    if (normal.y != 0.0) {
        float fMin = mix(0.7, 0.98, 1.0 - props.light_intensity);
        float factor = clamp((t + base) * pow(height / 150.0, 0.5), fMin, 1.0);
        directional *= (1.0 - props.vertical_gradient) + (props.vertical_gradient * factor);
    }

    vec3 light_color = props.light_color_pad.rgb;
    vec3 minLight = mix(vec3(0.0), vec3(0.3), 1.0 - light_color);
    v_color += vec4(clamp(color.rgb * directional * light_color, minLight, vec3(1.0)), 0.0);
    v_color *= props.opacity;

    // (1 - n.L): 0 on sun-facing faces, ->1 on faces turned away; scales the depth bias so a
    // building never shadows its OWN away-faces.
    v_slope = 1.0 - directionalFraction;
    // 1.0 on vertical walls (normal.z~0), 0.0 on roofs (normal.z~+-1).
    v_wallness = 1.0 - abs(normal.z);
    v_cascade_count = drawable.cascade_count;

    // Project into every cascade's light clip (near->far). Unused slots replicate cascade 0.
    v_shadow_pos[0] = drawable.light_matrix[0] * worldLocal;
    v_shadow_pos[1] = drawable.light_matrix[drawable.cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = drawable.light_matrix[drawable.cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = drawable.light_matrix[drawable.cascade_count > 3 ? 3 : 0] * worldLocal;
}

)";

    static constexpr auto fragment = R"(

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_shadow_pos[4];
layout(location = 5) in float v_slope;
layout(location = 6) in float v_wallness;
layout(location = 7) flat in int v_cascade_count;

layout(location = 0) out vec4 out_color;

layout(set = LAYER_SET_INDEX, binding = idFillExtrusionShadowPropsUBO) uniform FillExtrusionShadowPropsUBO {
    vec4 color;
    vec4 light_color_pad;
    vec4 light_position_base;
    float height;
    float light_intensity;
    float vertical_gradient;
    float opacity;
    float shadow_intensity;
    float shadow_texel_size;
    float shadow_bias;
    float shadow_slope_bias;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D shadow0_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 1) uniform sampler2D shadow1_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 2) uniform sampler2D shadow2_sampler;
layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 3) uniform sampler2D shadow3_sampler;

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
float fe_pcfBilinear(sampler2D tex, vec2 uv, float texel, float current) {
    vec2 tc = uv / texel - 0.5;
    vec2 base = floor(tc);
    vec2 f = tc - base;
    vec2 c00 = (base + 0.5) * texel;
    float s00 = (current <= fe_unpackShadowDepth(texture(tex, c00))) ? 1.0 : 0.0;
    float s10 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(texel, 0.0)))) ? 1.0 : 0.0;
    float s01 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(0.0, texel)))) ? 1.0 : 0.0;
    float s11 = (current <= fe_unpackShadowDepth(texture(tex, c00 + vec2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

// Returns lit in [0,1] when this cascade contains the fragment, or -1.0 when it doesn't.
float fe_cascade(sampler2D tex, vec4 sp, int cIdx) {
    vec3 ndc = sp.xyz / sp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5; // no uv.y flip (GLES/Vulkan convention)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    // UNIFORM bias scale across cascades. The near cascade's tighter frustum means MORE depth
    // precision per texel, not less, so it must never carry more bias than the far cascade — a
    // larger near-cascade scale delays roof-received shadow onset exactly where precision is
    // highest. Building receiver only; walls are separately suppressed, so bias here can't leak
    // onto walls.
    float biasScale = 4.0;
    float current = ndc.z - (props.shadow_bias + v_slope * props.shadow_slope_bias) * biasScale;
    float l = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            l += fe_pcfBilinear(
                tex, uv + (vec2(float(dx), float(dy)) - 0.5) * props.shadow_texel_size, props.shadow_texel_size, current);
        }
    }
    return l / 4.0;
}

void main() {
    vec4 color = v_color;

    // CASCADED SHADOW MAPS: tightest containing cascade wins (hard transition). Vulkan GLSL cannot
    // dynamically index distinct sampler bindings, so the near->far walk is a static if-chain.
    float lit = 1.0;
    float r = -1.0;
    if (v_cascade_count > 0) {
        r = fe_cascade(shadow0_sampler, v_shadow_pos[0], 0);
        if (r >= 0.0) {
            lit = r;
        } else if (v_cascade_count > 1) {
            r = fe_cascade(shadow1_sampler, v_shadow_pos[1], 1);
            if (r >= 0.0) {
                lit = r;
            } else if (v_cascade_count > 2) {
                r = fe_cascade(shadow2_sampler, v_shadow_pos[2], 2);
                if (r >= 0.0) {
                    lit = r;
                } else if (v_cascade_count > 3) {
                    r = fe_cascade(shadow3_sampler, v_shadow_pos[3], 3);
                    if (r >= 0.0) {
                        lit = r;
                    }
                }
            }
        }
    }

    // Suppress the (projective-aliased) cast shadow on near-vertical walls; keep it on roofs.
    lit = mix(lit, 1.0, smoothstep(0.4, 0.85, v_wallness));
    color.rgb *= (1.0 - (1.0 - lit) * props.shadow_intensity);
    out_color = color;
}

)";
};

} // namespace shaders
} // namespace mbgl
