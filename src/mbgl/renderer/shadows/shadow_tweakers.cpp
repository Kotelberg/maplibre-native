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
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/tile_coordinate.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

double tileWorldZScale(const TransformState& state, const OverscaledTileID& tileID) {
    if (!std::getenv("MLN_GROUND_SHADOWS")) {
        return 1.0;
    }
    const uint64_t tileScale = 1ull << tileID.overscaledZ;
    return Projection::worldSize(state.getScale()) / static_cast<double>(tileScale) / util::EXTENT;
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

vec3 centerPixelToWorld(const TransformState& state, uint8_t tileZoom) {
    const Size size = state.getSize();
    const ScreenCoordinate centerPixel = {0.5 * size.width, 0.5 * size.height};
    const TileCoordinate centerCoord = state.screenCoordinateToTileCoordinate(centerPixel, tileZoom);

    const auto tileX = static_cast<int64_t>(std::floor(centerCoord.p.x));
    const auto tileY = static_cast<int64_t>(std::floor(centerCoord.p.y));
    const double localX = (centerCoord.p.x - static_cast<double>(tileX)) * util::EXTENT;
    const double localY = (centerCoord.p.y - static_cast<double>(tileY)) * util::EXTENT;
    const UnwrappedTileID tileID(tileZoom, tileX, tileY);

    mat4 tileWorld;
    state.matrixFor(tileWorld, tileID);
    return tileCornerToWorld(tileWorld, localX, localY);
}

Point<double> heightCompensatedFootprintCenter(const vec3& focalCenter, const vec3& sunDir, double maxHeightWorld) {
    const mat4 lightView = ShadowFrustum::lightView(sunDir);
    const double a = lightView[0];
    const double b = lightView[4];
    const double c = lightView[1];
    const double d = lightView[5];
    const double det = a * d - b * c;
    if (std::abs(det) < 1e-9) {
        return {focalCenter[0], focalCenter[1]};
    }

    const double rhsX = -0.5 * maxHeightWorld * lightView[8];
    const double rhsY = -0.5 * maxHeightWorld * lightView[9];
    const double dx = (rhsX * d - b * rhsY) / det;
    const double dy = (a * rhsY - rhsX * c) / det;
    return {focalCenter[0] + dx, focalCenter[1] + dy};
}

} // namespace

mat4 computeWorldToLightClip(LayerGroupBase& layerGroup, const PaintParameters& parameters, uint32_t mapSize) {
    const auto& state = parameters.state;
    const vec3 sunDir = ShadowSun::direction(parameters.evaluatedLight.get<LightPosition>(),
                                             parameters.evaluatedLight.get<LightAnchor>(),
                                             static_cast<float>(state.getBearing()));

    // Footprint = the world-space rects of every tile currently drawn in this layer group.
    std::vector<vec3> ground;
    double maxLightHeightScale = 0.0;
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        const auto& tileID = drawable.getTileID();
        if (!tileID) {
            return;
        }
        const UnwrappedTileID unwrapped = tileID->toUnwrapped();
        mat4 tileWorld;
        state.matrixFor(tileWorld, unwrapped);
        maxLightHeightScale = std::max(maxLightHeightScale, tileWorldZScale(state, *tileID));
        ground.push_back(tileCornerToWorld(tileWorld, 0.0, 0.0));
        ground.push_back(tileCornerToWorld(tileWorld, util::EXTENT, 0.0));
        ground.push_back(tileCornerToWorld(tileWorld, 0.0, util::EXTENT));
        ground.push_back(tileCornerToWorld(tileWorld, util::EXTENT, util::EXTENT));
    });

    if (ground.empty()) {
        mat4 identity;
        matrix::identity(identity);
        return identity;
    }

    double tileMeanX = 0.0, tileMeanY = 0.0;
    uint8_t focalZoom = 0;
    bool hasFocalZoom = false;
    for (const auto& p : ground) {
        tileMeanX += p[0];
        tileMeanY += p[1];
    }
    tileMeanX /= static_cast<double>(ground.size());
    tileMeanY /= static_cast<double>(ground.size());
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (hasFocalZoom) {
            return;
        }
        const auto& tileID = drawable.getTileID();
        if (!tileID) {
            return;
        }
        focalZoom = tileID->canonical.z;
        hasFocalZoom = true;
    });

    // Tighten the footprint to a bounded box around the map center. The full tile-cover
    // footprint stretches toward the horizon at high pitch, and its mean drifts away from
    // the focal buildings. Derive the focal point through TransformState::matrixFor(), the
    // same tile-local-to-world path used by the fitted tile corners and per-tile light_matrix.
    const LatLng cameraCenter = state.getLatLng(LatLng::Unwrapped);
    const Point<double> projectedCenter = Projection::project(cameraCenter, state.getScale());
    const vec3 focalCenter = centerPixelToWorld(state, focalZoom);
    const double maxHeightRaw = envFloat("MLN_SHADOW_MAX_HEIGHT", 200.0f);
    const double maxHeightWorld = maxHeightRaw * maxLightHeightScale;
    const Point<double> footprintCenter = heightCompensatedFootprintCenter(focalCenter, sunDir, maxHeightWorld);
    const double cx = footprintCenter.x;
    const double cy = footprintCenter.y;
    const double radius = envFloat("MLN_SHADOW_RADIUS", 700.0f);

    if (std::getenv("MLN_SHADOW_DBG")) {
        const double oldDx = tileMeanX - focalCenter[0];
        const double oldDy = tileMeanY - focalCenter[1];
        std::fprintf(stderr,
                     "MLN_SHADOW_DBG shadow_footprint pitch=%.2f zoom=%.2f points=%zu "
                     "old_centroid=(%.3f,%.3f) focal_world=(%.3f,%.3f) footprint_center=(%.3f,%.3f) "
                     "projected_center=(%.3f,%.3f) camera_center=(%.8f,%.8f) "
                     "focal_delta=(%.3f,%.3f) footprint_delta=(%.3f,%.3f) old_to_camera=%.3f radius=%.3f\n",
                     util::rad2deg(state.getPitch()),
                     state.getZoom(),
                     ground.size(),
                     tileMeanX,
                     tileMeanY,
                     focalCenter[0],
                     focalCenter[1],
                     footprintCenter.x,
                     footprintCenter.y,
                     projectedCenter.x,
                     projectedCenter.y,
                     cameraCenter.latitude(),
                     cameraCenter.longitude(),
                     focalCenter[0] - projectedCenter.x,
                     focalCenter[1] - projectedCenter.y,
                     footprintCenter.x - focalCenter[0],
                     footprintCenter.y - focalCenter[1],
                     std::hypot(oldDx, oldDy),
                     radius);
        std::fprintf(stderr,
                     "MLN_SHADOW_DBG shadow_height raw=%.3f z_scale=%.6f world=%.3f\n",
                     maxHeightRaw,
                     maxLightHeightScale,
                     maxHeightWorld);
    }

    const std::vector<vec3> footprint = {
        {cx - radius, cy - radius, 0.0}, {cx + radius, cy - radius, 0.0},
        {cx - radius, cy + radius, 0.0}, {cx + radius, cy + radius, 0.0}};

    const std::vector<vec3> pts = ShadowFrustum::heightExpand(footprint, maxHeightWorld);
    // Texel-snap the light frustum so the shadow-map sampling grid is stable in world space as
    // the camera pans/zooms/rotates — otherwise the shadow texels crawl ("shadow swimming") and
    // the shadows look unanchored from the buildings. Requires a world-fixed sun (map anchor).
    return ShadowFrustum::fit(sunDir, pts, mapSize, /*texelSnapEnabled=*/true);
}

void ShadowDepthTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const mat4 worldToLightClip = computeWorldToLightClip(layerGroup, parameters, mapSize);

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        const auto& tileID = drawable.getTileID();
        if (!tileID) {
            return;
        }
        mat4 tileWorld;
        matrixForLightTileWorld(tileWorld, parameters.state, *tileID);
        mat4 lightMatrix;
        matrix::multiply(lightMatrix, worldToLightClip, tileWorld);

        const ShadowDepthDrawableUBO ubo = {.light_matrix = util::cast<float>(lightMatrix)};
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

    const mat4 worldToLightClip = computeWorldToLightClip(layerGroup, parameters, mapSize);

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

    const mat4 worldToLightClip = computeWorldToLightClip(layerGroup, parameters, mapSize);

    const GroundShadowPropsUBO propsUBO = {.shadow_color = Color::black(),
                                           .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.5f),
                                           .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
                                           .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0015f),
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
