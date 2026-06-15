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
    // WORLD-ANCHORED light frustum. A directional sun does not move with the camera, so the shadow
    // map must cover the same world region regardless of camera PITCH (and bearing) — only the
    // look-at point and zoom select WHICH region. We therefore fit the frustum to a fixed-radius
    // square centered on the look-at ground point, NOT to the camera's view trapezoid.
    //
    // Why a fixed radius (not a view-frustum fit): the old per-frame view-fit made the shadow map's
    // coverage and its world->texel scale change with pitch/zoom, which (a) made shadows appear
    // pitch-dependent and (b) silently broke ShadowFrustum::fit's texel-snap (it assumes a constant
    // box size), so shadows swam and detached from buildings. A constant footprint restores both:
    // identical worldToLightClip across pitch (unit-tested) and a stable texel grid.
    //
    // World units here are TransformState::matrixFor() units == mercator screen-pixels at the current
    // scale, so 1 world unit == 1 screen pixel at the map center (definitional, pitch-independent).
    // Sizing the radius as a multiple of the on-screen extent therefore both (a) scales correctly
    // with zoom and (b) is exactly pitch-invariant. We deliberately cover the near/mid field around
    // the look-at point rather than the far horizon: far, barely-visible geometry should not consume
    // shadow-map resolution (and the user explicitly does not want shadows drawn far away at pitch).
    const uint8_t focalZoom = cameraFocalZoom(state);
    const vec3 focalCenter = centerPixelToWorld(state, focalZoom);
    const double maxHeightWorld = envFloat("MLN_SHADOW_MAX_HEIGHT", 200.0f) * shadowWorldZScale();

    const Size sz = state.getSize();
    const double screenExtent = 0.5 * std::hypot(static_cast<double>(sz.width), static_cast<double>(sz.height));
    const double radius = static_cast<double>(envFloat("MLN_SHADOW_RADIUS", 1.5f)) * screenExtent;

    const std::vector<vec3> footprint = {
        {focalCenter[0] - radius, focalCenter[1] - radius, 0.0},
        {focalCenter[0] + radius, focalCenter[1] - radius, 0.0},
        {focalCenter[0] - radius, focalCenter[1] + radius, 0.0},
        {focalCenter[0] + radius, focalCenter[1] + radius, 0.0},
    };

    if (std::getenv("MLN_SHADOW_DBG")) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "MLN_SHADOW_DBG pitch=%.1f zoom=%.2f focalZoom=%u size=%dx%d focal=(%.1f,%.1f) "
                      "radius=%.1f screenExtent=%.1f",
                      util::rad2deg(state.getPitch()), state.getZoom(), focalZoom, sz.width, sz.height,
                      focalCenter[0], focalCenter[1], radius, screenExtent);
        Log::Warning(Event::General, buf);
    }

    const std::vector<vec3> pts = ShadowFrustum::heightExpand(footprint, maxHeightWorld);
    // Texel-snap the light frustum so the shadow-map sampling grid is stable in world space as the
    // camera pans/zooms — the constant box size above is what makes the snap actually anchor the
    // grid (otherwise the texel size drifts and shadows crawl). Requires a world-fixed sun (map
    // anchor); see the style's light.anchor.
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
        .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.5f),
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
                                           .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.5f),
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
