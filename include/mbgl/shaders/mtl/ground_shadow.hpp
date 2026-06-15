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

fragment FragmentOutput fragmentMain(FragmentStage in [[stage_in]],
                                     device const GroundShadowPropsUBO& props [[buffer(idGroundShadowPropsUBO)]],
                                     texture2d<float, access::sample> shadowTexture [[texture(0)]]) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    const float3 ndc = in.shadow_pos.xyz / in.shadow_pos.w;
    const float2 uv = ndc.xy * 0.5 + 0.5;
    // UV-radial (frustum-edge) fade + view-depth (camera-distance) fade. The view-depth fade is
    // the primary near→far softener; the UV-radial fade only trims the very frustum rim.
    const float r = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0; // 0 at center, 1 at frustum edge
    const float uvFade = 1.0 - smoothstep(props.shadow_fade_start, 1.0, r);
    const float depthFade = ground_depthFade(in.view_w, props.depth_fade_start, props.depth_fade_end);
    const float fade = uvFade * depthFade;

    // UV-DEBUG (MLN_SHADOW_INTENSITY > 2.5): paint each ground fragment's shadow-map sample
    // coordinate so the screen can be correlated with the shadow-map dump. R=uv.x, G=uv.y;
    // B=1 where a caster is present at that uv (occl<0.99). Out-of-frustum uv -> magenta.
    if (props.shadow_intensity > 2.5) {
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            return {half4(1.0, 0.0, 1.0, 1.0)};
        }
        const float occlDbg = ground_unpackShadowDepth(shadowTexture.sample(shadowSampler, uv));
        return {half4(half(uv.x), half(uv.y), occlDbg < 0.99 ? half(1.0) : half(0.0), 1.0)};
    }

    // VIZ debug (toggle via MLN_SHADOW_INTENSITY > 1.5): paint frustum + caster coverage so the
    // live render reveals the cutoff cause (text logs don't surface in the RN host). blue = UV
    // outside the light frustum (no coverage); red = in frustum + a caster wrote depth (shadowed),
    // its intensity scaled by the SAME fade as the real shadow (so a band's red% reads the
    // EFFECTIVE shadow after the fade — faded shadow goes red→green, the lit color); green = in
    // frustum, no caster (lit ground).
    if (props.shadow_intensity > 1.5) {
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            return {half4(0.0, 0.0, 1.0, 0.55)};
        }
        const float occlV = ground_unpackShadowDepth(shadowTexture.sample(shadowSampler, uv));
        if (occlV < 0.99) {
            // red where the (post-fade) shadow is meaningful; ramp red→green as it fades so the
            // per-band red% reflects the effective shadow gradient, not raw caster coverage.
            const half3 vizColor = mix(half3(0.0, 1.0, 0.0), half3(1.0, 0.0, 0.0), half(fade));
            return {half4(vizColor, 0.6)};
        }
        return {half4(0.0, 1.0, 0.0, 0.35)};
    }
    // Sample the shadow map only when this ground fragment is inside the light frustum in ALL THREE
    // axes. The uv (xy) check alone is not enough: the ground quads span whole tiles that extend
    // past the bounded light frustum's depth range, so far ground points get ndc.z > 1 (beyond the
    // far plane) which exceeds the cleared depth (~1.0) and would falsely read as shadowed — a hard
    // diagonal of phantom shadow along the far-plane boundary. Points outside [0,1] in depth have no
    // possible occluder in the map, so they are lit. (Standard shadow-map receiver guard.)
    float lit = 1.0;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && ndc.z >= 0.0 && ndc.z <= 1.0) {
        const float current = ndc.z - props.shadow_bias;
        lit = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const float occl = ground_unpackShadowDepth(
                    shadowTexture.sample(shadowSampler, uv + float2(dx, dy) * props.shadow_texel_size));
                lit += (current <= occl) ? 1.0 : 0.0;
            }
        }
        lit /= 9.0;
    }

    return {half4(half3(props.shadow_color.rgb), half((1.0 - lit) * props.shadow_intensity * fade))};
}
)";
};

} // namespace shaders
} // namespace mbgl
