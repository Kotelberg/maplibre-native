#pragma once

#include <mbgl/shaders/fill_extrusion_shadow_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>
#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

// Shadow-receiving fill-extrusion shader. Mirrors the non-pattern FillExtrusionShader vertex
// lighting verbatim, adds a light-space position varying, and a fragment that PCF-samples the
// shadow map and attenuates the lit color. Non-consolidated single drawable UBO.
// On the INSTANCED path this shader receives shadows on the building ROOF (the sharedTriangles draw;
// footprint verts carry ed_discard, not normal_ed). Inject FE_INSTANCING so the roof uses the flat-cap
// constants normal=(0,0,1) / t=1 instead of the absent normal_ed. The walls draw via the plain
// instanced FE shader (their cast shadow is intentionally suppressed by wallness anyway).
constexpr auto fillExtrusionShadowShaderPrelude =
#if MLN_USE_FILL_EXTRUSION_INSTANCING
    "#define FE_INSTANCING 1\n"
#else
    "#define FE_INSTANCING 0\n"
#endif
    R"(

enum {
    idFillExtrusionShadowDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idFillExtrusionShadowPropsUBO = drawableReservedUBOCount,
    fillExtrusionShadowUBOCount
};

struct alignas(16) FillExtrusionShadowDrawableUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4x4 light_matrix[4]; // one per concentric cascade (near→far); first cascade_count valid
    /* 320 */ float base_t;
    /* 324 */ float height_t;
    /* 328 */ float color_t;
    /* 332 */ int cascade_count;
    // Per-tile building "grow-in" reveal factor [0,1]: scales base+height so a freshly-loaded tile's
    // buildings rise from the ground. 1 = full height. Caster stays full-height (see GL twin).
    /* 336 */ float height_grow;
    /* 340 */ float pad0;
    /* 344 */ float pad1;
    /* 348 */ float pad2;
    /* 352 */
};
static_assert(sizeof(FillExtrusionShadowDrawableUBO) == 22 * 16, "wrong size");

struct alignas(16) FillExtrusionShadowPropsUBO {
    /*  0 */ float4 color;
    /* 16 */ float4 light_color_pad;
    /* 32 */ float4 light_position_base;
    /* 48 */ float height;
    /* 52 */ float light_intensity;
    /* 56 */ float vertical_gradient;
    /* 60 */ float opacity;
    /* 64 */ float shadow_intensity;
    /* 68 */ float shadow_texel_size;
    /* 72 */ float shadow_bias;
    /* 76 */ float shadow_slope_bias; // extra bias scaled by (1 - n·L); kills self-shadowing
    /* 80 */
};
static_assert(sizeof(FillExtrusionShadowPropsUBO) == 5 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "FillExtrusionShadowShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    static const std::array<AttributeInfo, 4> attributes; // pos, color, base, height (roof; no normal_ed)
#else
    static const std::array<AttributeInfo, 5> attributes;
#endif
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 4> textures;

    static constexpr auto prelude = fillExtrusionShadowShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
#if FE_INSTANCING
    // Instanced roof receiver: no normal_ed; color/base/height shift down one slot.
#if !defined(HAS_UNIFORM_u_color)
    float4 color [[attribute(1)]];
#endif
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(2)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(3)]];
#endif
#else
    short4 normal_ed [[attribute(1)]];

#if !defined(HAS_UNIFORM_u_color)
    float4 color [[attribute(2)]];
#endif
#if !defined(HAS_UNIFORM_u_base)
    float2 base [[attribute(3)]];
#endif
#if !defined(HAS_UNIFORM_u_height)
    float2 height [[attribute(4)]];
#endif
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    half4 color;
    // Light-clip position per concentric cascade (near→far); only the first `cascade_count` are
    // valid. The fragment picks the tightest cascade that contains it (hard transition). NOTE: MSL
    // forbids array members in a vertex-output struct, so the 4 cascade slots are flattened here.
    float4 shadow_pos0;
    float4 shadow_pos1;
    float4 shadow_pos2;
    float4 shadow_pos3;
    // (1 - n·L): 0 on sun-facing faces, →1 on faces turned away from the sun. Scales the
    // depth bias so a building never shadows its OWN away-faces (the directional lighting
    // already darkens those); only a neighbour's cast shadow falling across it darkens it.
    float slope;
    // 1.0 on vertical walls (normal.z≈0), 0.0 on roofs/up-facing faces (normal.z≈±1). Used to
    // suppress the (aliased) cast shadow on walls while keeping it on roofs. Smoothly interpolated
    // across the roof→wall crease so there's no hard transition line.
    float wallness;
    int cascade_count [[flat]];
    // Per-tile grow-in factor [0,1]; fades the received shadow in as the building rises so a still-
    // short building isn't greyed by its own full-height caster shadow (see GL twin).
    float height_grow;
};

struct FragmentOutput {
    half4 color [[color(0)]];
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const FillExtrusionShadowDrawableUBO& drawable [[buffer(idFillExtrusionShadowDrawableUBO)]],
                                device const FillExtrusionShadowPropsUBO& props [[buffer(idFillExtrusionShadowPropsUBO)]]) {

#if defined(HAS_UNIFORM_u_base)
    auto base   = props.light_position_base.w;
#else
    auto base   = max(unpack_mix_float(vertx.base, drawable.base_t), 0.0);
#endif
#if defined(HAS_UNIFORM_u_height)
    auto height = props.height;
#else
    auto height = max(unpack_mix_float(vertx.height, drawable.height_t), 0.0);
#endif

    // Building "grow-in" reveal: scale the extrusion from the ground so a freshly-loaded tile rises.
    base   *= drawable.height_grow;
    height *= drawable.height_grow;

    // Instanced path: this receiver draws the flat roof cap (always the top), so the normal is the
    // up vector and t is 1. wallness = 1 - abs(normal.z) = 0 → roofs keep their crisp cast shadow.
#if FE_INSTANCING
    const float t = 1.0;
    const float3 normal = float3(0.0, 0.0, 1.0);
#else
    const float t = float(vertx.normal_ed.x & 1);
    const float3 normal = float3(vertx.normal_ed.xyz) / 16384.0;
#endif
    const float z = (t > 0.0) ? height : base;
    const float4 worldLocal = float4(float2(vertx.pos), z, 1);
    const float4 position = drawable.matrix * worldLocal;

#if defined(HAS_UNIFORM_u_color)
    auto color = props.color;
#else
    auto color = unpack_mix_color(vertx.color, drawable.color_t);
#endif

    const float luminance = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    float4 vcolor = float4(0.0, 0.0, 0.0, 1.0);
    color += min(float4(0.03, 0.03, 0.03, 1.0), float4(1.0));

    const float directionalFraction = clamp(dot(normal, props.light_position_base.xyz), 0.0, 1.0);
    const float minDirectional = 1.0 - props.light_intensity;
    const float maxDirectional = max(1.0 - luminance + props.light_intensity, 1.0);
    float directional = mix(minDirectional, maxDirectional, directionalFraction);

    if (normal.y != 0.0) {
        const float fMin = mix(0.7, 0.98, 1.0 - props.light_intensity);
        const float factor = clamp((t + base) * pow(height / 150.0, 0.5), fMin, 1.0);
        directional *= (1.0 - props.vertical_gradient) + (props.vertical_gradient * factor);
    }

    const float3 light_color = props.light_color_pad.rgb;
    const float3 minLight = mix(0.0, 0.3, 1.0 - light_color.rgb);
    vcolor += float4(clamp(color.rgb * directional * light_color.rgb, minLight, 1.0), 0.0);

    FragmentStage out;
    out.position = position;
    out.color = half4(vcolor * props.opacity);
    out.slope = 1.0 - directionalFraction;
    out.wallness = 1.0 - abs(normal.z); // roof normal=(0,0,1)→0; wall normal=(nx,ny,0)→1
    out.height_grow = drawable.height_grow;
    const int cc = drawable.cascade_count;
    out.cascade_count = cc;
    // Project into every cascade's light clip (near→far). Unused slots (c >= cascade_count) replicate
    // cascade 0 so nothing is left uninitialized; the fragment only reads c < cascade_count.
    out.shadow_pos0 = drawable.light_matrix[0] * worldLocal;
    out.shadow_pos1 = drawable.light_matrix[(cc > 1) ? 1 : 0] * worldLocal;
    out.shadow_pos2 = drawable.light_matrix[(cc > 2) ? 2 : 0] * worldLocal;
    out.shadow_pos3 = drawable.light_matrix[(cc > 3) ? 3 : 0] * worldLocal;
    return out;
}

float fe_unpackShadowDepth(float4 rgba) {
    return dot(rgba, float4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));
}

// Bilinear percentage-closer filter: depth-compare the 4 texels surrounding `uv`, then bilinearly
// blend the 0/1 results. The map stores RGBA8-PACKED depth, so hardware linear filtering would blend
// the bytes (garbage) — we must compare FIRST, then blend. This turns the cast-shadow edge from a
// per-texel staircase (which reads as stripes on a receiving rooftop/wall) into a smooth gradient.
float fe_pcfBilinear(texture2d<float, access::sample> tex, sampler s, float2 uv, float texel, float current) {
    const float2 tc = uv / texel - 0.5;
    const float2 base = floor(tc);
    const float2 f = tc - base;
    const float2 c00 = (base + 0.5) * texel;
    const float s00 = (current <= fe_unpackShadowDepth(tex.sample(s, c00))) ? 1.0 : 0.0;
    const float s10 = (current <= fe_unpackShadowDepth(tex.sample(s, c00 + float2(texel, 0.0)))) ? 1.0 : 0.0;
    const float s01 = (current <= fe_unpackShadowDepth(tex.sample(s, c00 + float2(0.0, texel)))) ? 1.0 : 0.0;
    const float s11 = (current <= fe_unpackShadowDepth(tex.sample(s, c00 + float2(texel, texel)))) ? 1.0 : 0.0;
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

fragment FragmentOutput fragmentMain(FragmentStage in [[stage_in]],
                                     device const FillExtrusionShadowPropsUBO& props [[buffer(idFillExtrusionShadowPropsUBO)]],
                                     array<texture2d<float, access::sample>, 4> shadowTextures [[texture(0)]]) {
    half4 color = in.color;
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, address::clamp_to_edge);
    // CASCADED SHADOW MAPS: walk cascades near→far and use the TIGHTEST (highest-resolution) one that
    // contains this fragment — i.e. its light-clip uv is inside [0,1] in all three axes. Hard
    // transition (no cross-cascade blend): the first containing cascade wins. A fragment outside every
    // cascade's bounded frustum is left fully lit.
    float lit = 1.0;
    for (int c = 0; c < in.cascade_count; ++c) {
        // Select this cascade's light-clip position (flattened varyings — MSL has no varying arrays).
        float4 sp = in.shadow_pos0;
        if (c == 1) sp = in.shadow_pos1;
        else if (c == 2) sp = in.shadow_pos2;
        else if (c == 3) sp = in.shadow_pos3;
        const float3 ndc = sp.xyz / sp.w;
        // Flip uv.y for Metal's top-left offscreen-texture origin (the caster writes the map with that
        // origin, so the receiver mirrors Y to sample the right texel).
        float2 uv = ndc.xy * 0.5 + 0.5;
        uv.y = 1.0 - uv.y;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
            continue; // not covered by this cascade — fall back to the next (wider) one
        }
        // Slope-scaled bias: away-from-sun faces get a large bias so they never self-shadow (their
        // shading is the directional light's job); sun-facing faces keep the small base bias so a
        // neighbour's cast shadow still lands on them.
        // PER-CASCADE bias scale: the near cascades have a much tighter frustum than the far one, so
        // the bias tuned for the far cascade is too small there and the building WALLS self-shadow
        // into dark blotches (the ground, being flat, never hits this). Scale the bias up on every
        // cascade EXCEPT the far one (c == cascade_count-1), which keeps its tuned value so legitimate
        // inter-building cast shadows still land. 8x matches the empirically-clean near-cascade bias.
        const float biasScale = (c < in.cascade_count - 1) ? 8.0 : 1.0;
        const float current = ndc.z - (props.shadow_bias + in.slope * props.shadow_slope_bias) * biasScale;
        // 2x2 grid of bilinear-PCF taps: smooth, staircase-free penumbra at 16 samples.
        float l = 0.0;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                l += fe_pcfBilinear(shadowTextures[c], shadowSampler,
                                    uv + (float2(dx, dy) - 0.5) * props.shadow_texel_size,
                                    props.shadow_texel_size, current);
            }
        }
        lit = l / 4.0;
        break; // tightest containing cascade wins (hard transition)
    }
    // Suppress the cast shadow on VERTICAL WALLS (keep it on roofs + the separate ground receiver).
    // Under a near-overhead sun a vertical wall is almost parallel to the light, so its shadow-map
    // sample is projective garbage — the map's silhouette (the building's own body / the skyline)
    // smears onto the wall (back wall: "ground shadow climbs the wall"; sun-facing wall: a dark
    // patch). Depth bias cannot fix this (it's an XY/projection issue, not a depth one). Walls still
    // get their directional face-shading; only the (broken) cast-shadow term is faded out. Roofs face
    // the light head-on (no aliasing) so they keep crisp cast shadows from taller neighbours.
    lit = mix(lit, 1.0, smoothstep(0.4, 0.85, in.wallness));
    // Building "grow-in" reveal: fade the received shadow in with the grow factor so a still-rising
    // building isn't greyed by its own full-height caster shadow (see GL twin).
    color.rgb *= half(1.0 - (1.0 - lit) * props.shadow_intensity * in.height_grow);
    return { color };
}
)";
};

} // namespace shaders
} // namespace mbgl
