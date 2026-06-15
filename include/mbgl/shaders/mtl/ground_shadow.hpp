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
    /*  64 */ float4x4 light_matrix;
    /* 128 */
};
static_assert(sizeof(GroundShadowDrawableUBO) == 8 * 16, "wrong size");

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
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = groundShadowShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float4 shadow_pos;
    // Clip-space w of this ground fragment = perspective view-distance from the camera
    // (world/mercator-px units). Used for the near→far view-depth fade. Interpolated
    // perspective-correctly by the rasterizer (it carries 1/w in the standard varying path,
    // so passing w directly is fine for a monotonic fade weight).
    float view_w;
};

struct FragmentOutput {
    half4 color [[color(0)]];
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GroundShadowDrawableUBO& drawable [[buffer(idGroundShadowDrawableUBO)]]) {
    const float4 worldLocal = float4(float2(vertx.pos), 0.0, 1.0);
    const float4 clip = drawable.matrix * worldLocal;
    return {
        .position = clip,
        .shadow_pos = drawable.light_matrix * worldLocal,
        .view_w = clip.w,
    };
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
                                     texture2d<float, access::sample> shadowTexture [[texture(0)]]) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    const float3 ndc = in.shadow_pos.xyz / in.shadow_pos.w;
    // Metal renders the shadow map into an offscreen texture whose origin is TOP-left, while
    // uv = ndc.xy*0.5+0.5 assumes a bottom-left origin. Flip uv.y so the receiver samples the texel
    // the caster actually wrote. Without this the sample is vertically mirrored in light space, so a
    // ground point only finds its caster where the mirror happens to coincide — the root cause of the
    // "shadows only in part of the screen" / anti-sun-only pattern. (The caster vertex path and the
    // receiver's matrix are identical; only the texture-coordinate convention differed.)
    float2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    // UV-radial (frustum-edge) fade + view-depth (camera-distance) fade. The view-depth fade is
    // the primary near→far softener; the UV-radial fade only trims the very frustum rim.
    const float r = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0; // 0 at center, 1 at frustum edge
    const float uvFade = 1.0 - smoothstep(props.shadow_fade_start, 1.0, r);
    const float depthFade = ground_depthFade(in.view_w, props.depth_fade_start, props.depth_fade_end);
    const float fade = uvFade * depthFade;

    // Sample the shadow map only when this ground fragment is inside the light frustum in ALL THREE
    // axes. The uv (xy) check alone is not enough: the ground quads span whole tiles that extend
    // past the bounded light frustum's depth range, so far ground points get ndc.z > 1 (beyond the
    // far plane) which exceeds the cleared depth (~1.0) and would falsely read as shadowed — a hard
    // diagonal of phantom shadow along the far-plane boundary. Points outside [0,1] in depth have no
    // possible occluder in the map, so they are lit. (Standard shadow-map receiver guard.)
    float lit = 1.0;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && ndc.z >= 0.0 && ndc.z <= 1.0) {
        const float current = ndc.z - props.shadow_bias;
        // 2x2 grid of bilinear-PCF taps: smooth, staircase-free penumbra (matches the building
        // receiver) so courtyard / inter-building ground shadows aren't blocky.
        lit = 0.0;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                lit += ground_pcfBilinear(shadowTexture, shadowSampler,
                                          uv + (float2(dx, dy) - 0.5) * props.shadow_texel_size,
                                          props.shadow_texel_size, current);
            }
        }
        lit /= 4.0;
    }

    return {half4(half3(props.shadow_color.rgb), half((1.0 - lit) * props.shadow_intensity * fade))};
}
)";
};

} // namespace shaders
} // namespace mbgl
