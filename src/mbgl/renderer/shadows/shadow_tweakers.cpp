#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

#include <mbgl/renderer/shadows/shadow_sun.hpp>
#include <mbgl/renderer/shadows/shadow_frustum.hpp>
#include <mbgl/shaders/shadow_depth_ubo.hpp>
#include <mbgl/shaders/fill_extrusion_shadow_ubo.hpp>
#include <mbgl/shaders/ground_shadow_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/map/transform_state.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_light.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/renderer/paint_property_binder.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_properties.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/tile_coordinate.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace mbgl {

using namespace shaders;
using namespace style;

namespace {

float envFloat(const char* name, float fallback) {
    if (const char* v = std::getenv(name)) {
        return static_cast<float>(std::atof(v));
    }
    return fallback;
}

double tileWorldZScale(const TransformState&, const OverscaledTileID&) {
    // The fill-extrusion height vertex is already in the same world units as the x/y
    // footprint (mercator pixels), so matrixFor's z-scale of 1 is isotropic and correct.
    // The real cause of the old over-long ground shadows was the caster front-face cull
    // dropping the roof (see render_fill_extrusion_layer.cpp), not the height scale.
    // Kept env-tunable (MLN_SHADOW_ZSCALE) only for on-device length dialing.
    return envFloat("MLN_SHADOW_ZSCALE", 1.0f);
}

double shadowWorldZScale() {
    return envFloat("MLN_SHADOW_ZSCALE", 1.0f);
}

void matrixForLightTileWorld(mat4& tileWorld, const TransformState& state, const OverscaledTileID& tileID) {
    state.matrixFor(tileWorld, tileID.toUnwrapped());
    const double zScale = tileWorldZScale(state, tileID);
    // Ground shadows need isotropic light space; the established building-only
    // path keeps zScale=1 when MLN_GROUND_SHADOWS is unset.
    matrix::scale(tileWorld, tileWorld, 1.0, 1.0, zScale);
}

// Transform a tile-local point (EXTENT units, z=0) into world space via the tile matrix.
vec3 tileCornerToWorld(const mat4& tileWorld, double x, double y) {
    vec4 out;
    matrix::transformMat4(out, vec4{{x, y, 0.0, 1.0}}, tileWorld);
    return {out[0] / out[3], out[1] / out[3], out[2] / out[3]};
}

vec3 screenPixelToWorld(const TransformState& state, uint8_t tileZoom, double px, double py) {
    const TileCoordinate coord = state.screenCoordinateToTileCoordinate({px, py}, tileZoom);
    const auto tileX = static_cast<int64_t>(std::floor(coord.p.x));
    const auto tileY = static_cast<int64_t>(std::floor(coord.p.y));
    const double localX = (coord.p.x - static_cast<double>(tileX)) * util::EXTENT;
    const double localY = (coord.p.y - static_cast<double>(tileY)) * util::EXTENT;
    const UnwrappedTileID tileID(tileZoom, tileX, tileY);

    mat4 tileWorld;
    state.matrixFor(tileWorld, tileID);
    return tileCornerToWorld(tileWorld, localX, localY);
}

vec3 centerPixelToWorld(const TransformState& state, uint8_t tileZoom) {
    const Size size = state.getSize();
    return screenPixelToWorld(state, tileZoom, 0.5 * size.width, 0.5 * size.height);
}

uint8_t cameraFocalZoom(const TransformState& state) {
    return std::min(state.getIntegerZoom(), util::DEFAULT_MAX_ZOOM);
}

const mat4& worldToLightClipForFrame(ShadowFrustumState& frustumState,
                                     const PaintParameters& parameters,
                                     uint32_t mapSize) {
    if (!frustumState.valid || frustumState.frameCount != parameters.frameCount || frustumState.mapSize != mapSize) {
        frustumState.worldToLightClip = computeWorldToLightClip(parameters, mapSize);
        frustumState.frameCount = parameters.frameCount;
        frustumState.mapSize = mapSize;
        frustumState.valid = true;
    }
    return frustumState.worldToLightClip;
}

} // namespace

mat4 computeWorldToLightClip(const PaintParameters& parameters, uint32_t mapSize) {
    const auto& state = parameters.state;
    const vec3 sunDir = ShadowSun::direction(parameters.evaluatedLight.get<LightPosition>(),
                                             parameters.evaluatedLight.get<LightAnchor>(),
                                             static_cast<float>(state.getBearing()));
    return computeWorldToLightClip(state, sunDir, mapSize);
}

mat4 computeWorldToLightClip(const TransformState& state, const vec3& sunDir, uint32_t mapSize) {
    // WORLD-ANCHORED light frustum, sized to COVER the visible ground but BEARING-INVARIANT so the
    // shadows don't move when the camera rotates (the user's core requirement).
    //
    // The hard part of map shadows is the pitched view: the visible ground is a forward trapezoid
    // that, at steep pitch, reaches thousands of world-units past the look-at point. A frustum sized
    // to the FLAT on-screen extent leaves that far field uncovered -> "shadows only in the bottom
    // half". A view-frustum fit would cover it but rotates WITH the camera, so the shadow texels
    // crawl as you turn -> shadows appear to move. We resolve both: take the coverage RADIUS to be
    // the farthest the visible ground reaches from the look-at point (so it grows with pitch and the
    // far field is covered), but apply it as a SYMMETRIC square around the look-at point. The radius
    // is a scalar (max distance), hence invariant to bearing: rotating the camera does not change it,
    // so the frustum — and the shadows — stay put under rotation. Capped at MLN_SHADOW_MAX_DIST so a
    // horizon-grazing steep view doesn't blow the frustum up (and far, barely-visible geometry is
    // intentionally left uncovered, per the user).
    //
    // World units are TransformState::matrixFor() units == mercator screen-pixels at the current
    // scale (1 world unit == 1 screen pixel at the map center), so all distances below scale with
    // zoom automatically.
    const uint8_t focalZoom = cameraFocalZoom(state);
    const vec3 focalCenter = centerPixelToWorld(state, focalZoom);
    const double maxHeightWorld = envFloat("MLN_SHADOW_MAX_HEIGHT", 200.0f) * shadowWorldZScale();

    const Size sz = state.getSize();
    const double screenExtent = 0.5 * std::hypot(static_cast<double>(sz.width), static_cast<double>(sz.height));
    // Floor: at flat pitch the visible field is ~the screen; keep at least this much coverage.
    const double minRadius = static_cast<double>(envFloat("MLN_SHADOW_MIN_RADIUS", 1.2f)) * screenExtent;
    // Cap: world-distance ceiling on coverage (keeps resolution sane + drops the far horizon).
    const double maxDist = static_cast<double>(envFloat("MLN_SHADOW_MAX_DIST", 4000.0f));
    // Corner safety: the top screen CORNERS reach a bit farther than the top-center; pad the radius
    // so the far corners of the pitched view are still covered.
    const double cornerFactor = static_cast<double>(envFloat("MLN_SHADOW_CORNER_FACTOR", 1.3f));

    // Bearing-invariant coverage radius. The binding constraint at pitch is the FORWARD reach of the
    // visible ground. Sample DOWN the screen's vertical center column and take the farthest ground
    // intersection from the look-at point: rows above the horizon unproject to the near plane (a
    // small distance — screenCoordinateToTileCoordinate returns the near point when the ray escapes
    // the ground), while the row just below the horizon gives the true far reach, which peaks there.
    // That maximum distance is a function of pitch / fov / zoom only — NOT of the compass direction —
    // because rotating the camera only spins this centerline; its far-reach magnitude is unchanged.
    // So the radius, and the whole symmetric frustum, stay invariant under rotation. Capped at
    // maxDist so a near-horizon row can't blow the frustum up (and the far horizon is left uncovered,
    // per the user). cornerFactor pads for the wider far CORNERS of the view.
    double reach = 0.0;
    {
        const double fy[6] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
        for (double f : fy) {
            const vec3 w = screenPixelToWorld(state, focalZoom, 0.5 * sz.width, f * sz.height);
            const double d = std::hypot(w[0] - focalCenter[0], w[1] - focalCenter[1]);
            if (std::isfinite(d)) {
                reach = std::max(reach, std::min(d, maxDist));
            }
        }
    }
    const double radius = std::min(std::max(minRadius, reach * cornerFactor), maxDist);

    // Footprint aligned to the SUN's GROUND axes, so that in light space it is an axis-aligned
    // square that fills the whole shadow map. A WORLD-axis square (focalCenter ± radius in world x/y)
    // becomes a 45°-rotated DIAMOND in light space once the sun rotates the light basis, so its
    // light-space AABB is ~2x the diamond — the casters land in the inscribed diamond and the four
    // uv-corner triangles are empty. Ground sampling those corners reads cleared/empty map and is
    // (wrongly) lit. That wasted-half is the dominant cause of "shadows only in part of the screen".
    // The two sun-ground axes are orthonormal: rightG ⊥ the sun's ground direction (matches
    // ShadowFrustum::lightView's `right`), sunG = the sun's ground direction. A square of half-side
    // `radius` along these axes still contains the full visible disk of radius `radius` (a square of
    // half-side R contains the disk of radius R), so coverage is preserved while the map is filled.
    const double fwdHyp = std::hypot(sunDir[0], sunDir[1]);
    double rgx = 1.0, rgy = 0.0, sgx = 0.0, sgy = 1.0; // overhead-sun fallback: world axes
    if (fwdHyp > 1e-4) {
        // fwd = -sunDir; right = normalize(cross({0,0,1}, fwd)).xy = normalize(-fwd.y, fwd.x)
        // = normalize(sunDir.y, -sunDir.x); sunG = normalize(fwd.xy) = normalize(-sunDir.xy).
        rgx = sunDir[1] / fwdHyp;
        rgy = -sunDir[0] / fwdHyp;
        sgx = -sunDir[0] / fwdHyp;
        sgy = -sunDir[1] / fwdHyp;
    }
    const std::vector<vec3> footprint = {
        {focalCenter[0] - radius * rgx - radius * sgx, focalCenter[1] - radius * rgy - radius * sgy, 0.0},
        {focalCenter[0] + radius * rgx - radius * sgx, focalCenter[1] + radius * rgy - radius * sgy, 0.0},
        {focalCenter[0] - radius * rgx + radius * sgx, focalCenter[1] - radius * rgy + radius * sgy, 0.0},
        {focalCenter[0] + radius * rgx + radius * sgx, focalCenter[1] + radius * rgy + radius * sgy, 0.0},
    };

    if (std::getenv("MLN_SHADOW_DBG")) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "MLN_SHADOW_DBG pitch=%.1f zoom=%.2f focalZoom=%u size=%dx%d focal=(%.1f,%.1f) "
                      "radius=%.1f screenExtent=%.1f maxDist=%.0f",
                      util::rad2deg(state.getPitch()), state.getZoom(), focalZoom, sz.width, sz.height,
                      focalCenter[0], focalCenter[1], radius, screenExtent, maxDist);
        Log::Warning(Event::General, buf);
    }

    const std::vector<vec3> pts = ShadowFrustum::heightExpand(footprint, maxHeightWorld);
    // Texel-snap the light frustum so the shadow-map sampling grid is stable in world space as the
    // camera pans/rotates. The symmetric radius keeps the box bearing-invariant; the snap then
    // anchors the grid so shadows don't crawl. (Pitch changes resize the box — a brief, rare
    // transient.) Requires a world-fixed sun (map anchor); see the style's light.anchor.
    return ShadowFrustum::fit(sunDir, pts, mapSize, /*texelSnapEnabled=*/true);
}

void ShadowDepthTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const auto& state = parameters.state;
    const mat4& worldToLightClip = worldToLightClipForFrame(*frustumState, parameters, mapSize);

    // Constant base/height fallbacks for caster drawables whose base/height is uniform
    // (HAS_UNIFORM_u_base/u_height). These mirror the visible FE shader's
    // props.light_position_base.w / props.height so the caster extrudes to the same height.
    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;
    const float constBase = evaluated.get<FillExtrusionBase>().constantOr(0.0f);
    const float constHeight = evaluated.get<FillExtrusionHeight>().constantOr(0.0f);
    const auto zoom = static_cast<float>(state.getZoom());

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        const auto& tileID = drawable.getTileID();
        if (!tileID) {
            return;
        }
        mat4 tileWorld;
        matrixForLightTileWorld(tileWorld, state, *tileID);
        mat4 lightMatrix;
        matrix::multiply(lightMatrix, worldToLightClip, tileWorld);

        // Per-vertex interpolation factor for the data-driven base/height path: identical to
        // the receiver (shadow_tweakers.cpp FillExtrusionShadowTweaker) and the visible FE
        // layer tweaker, so caster height == visible building height at every fractional zoom.
        float baseT = 0.0f;
        float heightT = 0.0f;
        if (auto* binders = static_cast<FillExtrusionBinders*>(drawable.getBinders())) {
            baseT = std::get<0>(binders->get<FillExtrusionBase>()->interpolationFactor(zoom));
            heightT = std::get<0>(binders->get<FillExtrusionHeight>()->interpolationFactor(zoom));
        }

        const ShadowDepthDrawableUBO ubo = {.light_matrix = util::cast<float>(lightMatrix),
                                            .base_t = baseT,
                                            .height_t = heightT,
                                            .u_base = constBase,
                                            .u_height = constHeight};
        drawable.mutableUniformBuffers().createOrUpdate(idShadowDepthDrawableUBO, &ubo, context);
    });
}

void FillExtrusionShadowTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const auto& state = parameters.state;
    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;

    const mat4& worldToLightClip = worldToLightClipForFrame(*frustumState, parameters, mapSize);

    // Per-layer props (shared across drawables).
    const auto lightColor = FillExtrusionBucket::lightColor(parameters.evaluatedLight);
    const auto lightPos = FillExtrusionBucket::lightPosition(parameters.evaluatedLight, state);
    const float base = evaluated.get<FillExtrusionBase>().constantOr(0.0f);
    const FillExtrusionShadowPropsUBO propsUBO = {
        .color = evaluated.get<FillExtrusionColor>().constantOr(Color::black()),
        .light_color_pad = {lightColor[0], lightColor[1], lightColor[2], 0.0f},
        .light_position_base = {lightPos[0], lightPos[1], lightPos[2], base},
        .height = evaluated.get<FillExtrusionHeight>().constantOr(0.0f),
        .light_intensity = FillExtrusionBucket::lightIntensity(parameters.evaluatedLight),
        .vertical_gradient = evaluated.get<FillExtrusionVerticalGradient>() ? 1.0f : 0.0f,
        .opacity = evaluated.get<FillExtrusionOpacity>(),
        // World-anchored shadow strength: constant at every pitch. A directional light's shadows do
        // not depend on the camera; the previous pitch-fade was a band-aid for a camera-coupled
        // frustum that no longer exists. Far/barely-visible geometry simply falls outside the
        // bounded light frustum (no coverage), so there is nothing to fade by pitch.
        .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.32f),
        .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
        .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0015f),
        // Default 0: let buildings self-shadow their away-from-sun faces (the crisp per-face
        // look the user wants — Mapbox does this). The slope term stays env-tunable
        // (MLN_SHADOW_SLOPE_BIAS) to suppress acne if a build ever needs it.
        .shadow_slope_bias = envFloat("MLN_SHADOW_SLOPE_BIAS", 0.0f)};
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    layerUniforms.createOrUpdate(idFillExtrusionShadowPropsUBO, &propsUBO, context);

    const auto zoom = static_cast<float>(state.getZoom());

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        const auto& tileID = drawable.getTileID();
        if (!tileID || !checkTweakDrawable(drawable)) {
            return;
        }
        auto* binders = static_cast<FillExtrusionBinders*>(drawable.getBinders());
        if (!binders) {
            return;
        }

        const auto& translation = evaluated.get<FillExtrusionTranslate>();
        const auto anchor = evaluated.get<FillExtrusionTranslateAnchor>();
        const mat4 matrix = getTileMatrix(
            tileID->toUnwrapped(), parameters, translation, anchor, /*nearClipped=*/true,
            /*inViewportPixelUnits=*/false, drawable);

        mat4 tileWorld;
        matrixForLightTileWorld(tileWorld, state, *tileID);
        mat4 lightMatrix;
        matrix::multiply(lightMatrix, worldToLightClip, tileWorld);

        const FillExtrusionShadowDrawableUBO ubo = {
            .matrix = util::cast<float>(matrix),
            .light_matrix = util::cast<float>(lightMatrix),
            .base_t = std::get<0>(binders->get<FillExtrusionBase>()->interpolationFactor(zoom)),
            .height_t = std::get<0>(binders->get<FillExtrusionHeight>()->interpolationFactor(zoom)),
            .color_t = std::get<0>(binders->get<FillExtrusionColor>()->interpolationFactor(zoom)),
            .pad1 = 0.0f};
        drawable.mutableUniformBuffers().createOrUpdate(idFillExtrusionShadowDrawableUBO, &ubo, context);
    });
}

void GroundShadowTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const auto& state = parameters.state;
    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;

    const mat4& worldToLightClip = worldToLightClipForFrame(*frustumState, parameters, mapSize);

    const GroundShadowPropsUBO propsUBO = {.shadow_color = Color::black(),
                                           // World-anchored: constant strength at every pitch (see
                                           // FillExtrusionShadowTweaker). No pitch fade.
                                           .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.32f),
                                           .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
                                           .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0015f),
                                           // UV-radial frustum-rim fade: softens the hard edge of the
                                           // bounded light frustum (a fixed WORLD radius around the
                                           // look-at point) so coverage tapers out in world space, not
                                           // by pitch. Default 0.92; MLN_SHADOW_FADE_START=1.0 to off.
                                           .shadow_fade_start = envFloat("MLN_SHADOW_FADE_START", 0.92f),
                                           // View-depth (camera-distance) fade removed — it was a
                                           // pitch-gated band-aid. Disabled (0/0): the shader's
                                           // ground_depthFade returns 1.0 when fade_end<=0.
                                           .depth_fade_start = 0.0f,
                                           .depth_fade_end = 0.0f,
                                           .pad0 = 0.0f,
                                           .pad1 = 0.0f};

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        // CRITICAL: only touch the ground-shadow quads (which have no paint binders). The
        // building drawables share this layer group, and the GroundShadow UBO ids ALIAS the
        // FillExtrusionShadow UBO ids (each shader's UBOs start at the same per-shader base
        // slot). Writing the ground UBOs onto a building drawable would clobber its
        // FillExtrusionShadowDrawableUBO (losing base_t/height_t → buildings flatten) and its
        // props. Buildings always carry FillExtrusionBinders; the quads never do.
        if (drawable.getBinders()) {
            return;
        }
        const auto& tileID = drawable.getTileID();
        if (!tileID || !checkTweakDrawable(drawable)) {
            return;
        }

        const auto& translation = evaluated.get<FillExtrusionTranslate>();
        const auto anchor = evaluated.get<FillExtrusionTranslateAnchor>();
        const mat4 matrix = getTileMatrix(tileID->toUnwrapped(),
                                          parameters,
                                          translation,
                                          anchor,
                                          /*nearClipped=*/true,
                                          /*inViewportPixelUnits=*/false,
                                          drawable);

        mat4 tileWorld;
        matrixForLightTileWorld(tileWorld, state, *tileID);
        mat4 lightMatrix;
        matrix::multiply(lightMatrix, worldToLightClip, tileWorld);

        const GroundShadowDrawableUBO ubo = {.matrix = util::cast<float>(matrix),
                                             .light_matrix = util::cast<float>(lightMatrix)};
        // Props set per-drawable (NOT on the shared group) so they don't collide with the
        // FillExtrusionShadow group props at the aliased slot.
        auto& uniforms = drawable.mutableUniformBuffers();
        uniforms.createOrUpdate(idGroundShadowDrawableUBO, &ubo, context);
        uniforms.createOrUpdate(idGroundShadowPropsUBO, &propsUBO, context);
    });
}

} // namespace mbgl
