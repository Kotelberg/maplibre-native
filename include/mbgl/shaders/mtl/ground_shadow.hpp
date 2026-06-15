#pragma once

#include <mbgl/shaders/ground_shadow_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto groundShadowShaderPrelude = R"(

enum {
    idGroundShadowDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idGroundShadowPropsUBO = drawableReservedUBOCount,
    groundShadowUBOCount
};

struct alignas(16) GroundShadowDrawableUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4x4 light_matrix[4]; // one per concentric cascade (near→far); first cascade_count valid
    /* 320 */ int cascade_count;
    /* 324 */ float pad0;
    /* 328 */ float pad1;
    /* 332 */ float pad2;
    /* 336 */
};
static_assert(sizeof(GroundShadowDrawableUBO) == 21 * 16, "wrong size");

struct alignas(16) GroundShadowPropsUBO {
    /*  0 */ float4 shadow_color;
    /* 16 */ float shadow_intensity;
    /* 20 */ float shadow_texel_size;
    /* 24 */ float shadow_bias;
    /* 28 */ float shadow_fade_start;
    /* 32 */ float depth_fade_start;
    /* 36 */ float depth_fade_end;
    /* 40 */ float pad0;
    /* 44 */ float pad1;
    /* 48 */
};
static_assert(sizeof(GroundShadowPropsUBO) == 3 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "GroundShadowShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 4> textures;

    static constexpr auto prelude = groundShadowShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    // Light-clip position per concentric cascade (near→far); only the first `cascade_count` valid.
    // Flattened (MSL forbids array members in a vertex-output struct).
    float4 shadow_pos0;
    float4 shadow_pos1;
    float4 shadow_pos2;
    float4 shadow_pos3;
    // Clip-space w of this ground fragment = perspective view-distance from the camera
    // (world/mercator-px units). Used for the near→far view-depth fade. Interpolated
    // perspective-correctly by the rasterizer (it carries 1/w in the standard varying path,
    // so passing w directly is fine for a monotonic fade weight).
    float view_w;
    int cascade_count [[flat]];
};

struct FragmentOutput {
    half4 color [[color(0)]];
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GroundShadowDrawableUBO& drawable [[buffer(idGroundShadowDrawableUBO)]]) {
    const float4 worldLocal = float4(float2(vertx.pos), 0.0, 1.0);
    const float4 clip = drawable.matrix * worldLocal;
    FragmentStage out;
    out.position = clip;
    out.view_w = clip.w;
    const int cc = drawable.cascade_count;
    out.cascade_count = cc;
    // Project the ground point into every cascade's light clip (near→far). Unused slots replicate
    // cascade 0; the fragment only reads c < cascade_count (+ the last for the rim fade).
    out.shadow_pos0 = drawable.light_matrix[0] * worldLocal;
    out.shadow_pos1 = drawable.light_matrix[(cc > 1) ? 1 : 0] * worldLocal;
    out.shadow_pos2 = drawable.light_matrix[(cc > 2) ? 2 : 0] * worldLocal;
    out.shadow_pos3 = drawable.light_matrix[(cc > 3) ? 3 : 0] * worldLocal;
    return out;
}

float ground_unpackShadowDepth(float4 rgba) {
    return dot(rgba, float4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

// View-depth fade weight: 1 in the near field, ramping to 0 as the ground fragment's
// camera distance (clip-space w) crosses [depth_fade_start, depth_fade_end]. This is the
// primary far-fade: it turns ANY far deficit (frustum edge, caster-tile horizon, or the
// padding-miscentered grazing top rows at steep pitch) into a smooth near→far fade with no
// hard line, while leaving the near field (small w) at full strength. Disabled (returns 1)
// when depth_fade_end<=0 so the low-pitch / off path is unaffected.
float ground_depthFade(float view_w, float fade_start, float fade_end) {
    if (fade_end <= 0.0 || fade_end <= fade_start) {
        return 1.0;
    }
    return 1.0 - smoothstep(fade_start, fade_end, view_w);
}

// Bilinear percentage-closer filter (see fill_extrusion_shadow.hpp): compare the 4 texels around
// `uv` then bilinearly blend the 0/1 results — the map is RGBA8-PACKED depth so we must compare
// first, then blend. Smooths the per-texel staircase so complex shadow shapes (e.g. a building
// ring self-shadowing its own courtyard floor) read as a smooth gradient, not hard blocky edges.
float ground_pcfBilinear(texture2d<float, access::sample> tex, sampler s, float2 uv, float texel, float current) {
    const float2 tc = uv / texel - 0.5;
    const float2 base = floor(tc);
    const float2 f = tc - base;
    const float2 c00 = (base + 0.5) * texel;
    const float s00 = (current <= ground_unpackShadowDepth(tex.sample(s, c00))) ? 1.0 : 0.0;
    const float s10 = (current <= ground_unpackShadowDepth(tex.sample(s, c00 + float2(texel, 0.0)))) ? 1.0 : 0.0;
    const float s01 = (current <= ground_unpackShadowDepth(tex.sample(s, c00 + float2(0.0, texel)))) ? 1.0 : 0.0;
    const float s11 = (current <= ground_unpackShadowDepth(tex.sample(s, c00 + float2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

fragment FragmentOutput fragmentMain(FragmentStage in [[stage_in]],
                                     device const GroundShadowPropsUBO& props [[buffer(idGroundShadowPropsUBO)]],
                                     array<texture2d<float, access::sample>, 4> shadowTextures [[texture(0)]]) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    const int lastCascade = max(in.cascade_count - 1, 0);

    // UV-radial (frustum-edge) fade is tied to the FAR cascade — the OUTER coverage boundary — so the
    // tighter near cascade's inner edge never fades shadows mid-screen. (Metal's offscreen texture has
    // a TOP-left origin, so flip uv.y to match the texel the caster wrote.) The view-depth fade is
    // camera-distance based and cascade-independent.
    float4 farSp = in.shadow_pos0;
    if (lastCascade == 1) farSp = in.shadow_pos1;
    else if (lastCascade == 2) farSp = in.shadow_pos2;
    else if (lastCascade == 3) farSp = in.shadow_pos3;
    const float3 farNdc = farSp.xyz / farSp.w;
    float2 farUv = farNdc.xy * 0.5 + 0.5;
    farUv.y = 1.0 - farUv.y;
    const float r = max(abs(farUv.x - 0.5), abs(farUv.y - 0.5)) * 2.0; // 0 at center, 1 at far-frustum edge
    const float uvFade = 1.0 - smoothstep(props.shadow_fade_start, 1.0, r);
    const float depthFade = ground_depthFade(in.view_w, props.depth_fade_start, props.depth_fade_end);
    const float fade = uvFade * depthFade;

    // CASCADED SHADOW MAPS: walk cascades near→far and sample the TIGHTEST one that contains this
    // ground fragment (light-clip uv inside [0,1] in all three axes). The depth-range guard (ndc.z in
    // [0,1]) keeps far ground points beyond a cascade's far plane from reading phantom shadow; such
    // points fall through to the next (wider) cascade, or are lit if outside every cascade.
    float lit = 1.0;
    for (int c = 0; c < in.cascade_count; ++c) {
        float4 sp = in.shadow_pos0;
        if (c == 1) sp = in.shadow_pos1;
        else if (c == 2) sp = in.shadow_pos2;
        else if (c == 3) sp = in.shadow_pos3;
        const float3 ndc = sp.xyz / sp.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        uv.y = 1.0 - uv.y;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
            continue;
        }
        const float current = ndc.z - props.shadow_bias;
        // 2x2 grid of bilinear-PCF taps: smooth, staircase-free penumbra (matches the building
        // receiver) so courtyard / inter-building ground shadows aren't blocky.
        float l = 0.0;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                l += ground_pcfBilinear(shadowTextures[c], shadowSampler,
                                        uv + (float2(dx, dy) - 0.5) * props.shadow_texel_size,
                                        props.shadow_texel_size, current);
            }
        }
        lit = l / 4.0;
        break; // tightest containing cascade wins (hard transition)
    }

    return {half4(half3(props.shadow_color.rgb), half((1.0 - lit) * props.shadow_intensity * fade))};
}
)";
};

} // namespace shaders
} // namespace mbgl
