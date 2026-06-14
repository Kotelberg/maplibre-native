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

// Directional cast shadows are a 3D-view effect. At a flat / top-down camera they have no
// depth cue and, in dense areas, the (correct, full-length) ground shadows lie flat and
// clutter the map into a dark mess. Fade the cast-shadow strength out as the camera flattens
// — matching the app's pitch-gated 3D-building reveal — so a top-down view stays clean and
// shadows fade in smoothly as you tilt. Full strength by MLN_SHADOW_PITCH_FADE_HI (default
// 35°, well below the app's ~50° 3D pitch). Set LO>=HI or LO<0 to disable.
float pitchShadowFade(const TransformState& state) {
    const double deg = util::rad2deg(state.getPitch());
    const double lo = static_cast<double>(envFloat("MLN_SHADOW_PITCH_FADE_LO", 12.0f));
    const double hi = static_cast<double>(envFloat("MLN_SHADOW_PITCH_FADE_HI", 35.0f));
    if (lo < 0.0 || hi <= lo) {
        return 1.0f;
    }
    if (deg <= lo) {
        return 0.0f;
    }
    if (deg >= hi) {
        return 1.0f;
    }
    const double t = (deg - lo) / (hi - lo);
    return static_cast<float>(t * t * (3.0 - 2.0 * t)); // smoothstep
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

// View-depth fade bounds for the ground receiver, in clip-space w (= perspective view-distance
// from the camera, in world/mercator-px units). The ground vertex outputs clip.w = the tile->clip
// (perspective) w of the ground point; we fade the cast shadow from full at `start` to none at
// `end`. Both are expressed as multiples of a per-frame reference distance so they track zoom and
// pitch automatically:
//
//   refViewW ~= cameraToCenterDistance (screen px) * worldPerPixel (world units / screen px at the
//               focal ground point) = the view-w of the look-at ground point.
//
// Empirically (Phase-1 repro, p60 tall + device insets) the screen-center ground point sits at
// ~1x ref and the far/top rows that drop out sit a few x ref further; starting the fade and ending
// it a few x ref out makes the far thin smoothly with no hard line while leaving the entire near
// field (w < ref) untouched. Disabled when the multipliers collapse (end<=start) or at low pitch.
struct DepthFadeBounds {
    float start = 0.0f; // view-w where the fade begins (full shadow at/below this)
    float end = 0.0f;   // view-w where the shadow has fully faded (0 at/above this); <=0 disables
};

DepthFadeBounds computeDepthFadeBounds(const TransformState& state, uint8_t focalZoom) {
    DepthFadeBounds b;
    // Gate by pitch: the view-depth fade only addresses the steep-pitch far cut-off. At low pitch
    // the whole ground is at a similar (small) depth, the cut-off does not occur, and a depth fade
    // would only erode coverage — so disable it below MLN_SHADOW_DEPTH_FADE_PITCH (default 30°,
    // below the app's ~50° 3D pitch). Set the multipliers' end<=start to disable entirely.
    const double pitchDeg = util::rad2deg(state.getPitch());
    const double fadePitch = static_cast<double>(envFloat("MLN_SHADOW_DEPTH_FADE_PITCH", 30.0f));
    // Multipliers of refViewW (= cameraToCenterDistance * worldPerPixel = the view-w of the look-at
    // ground point). start=1.3 keeps the entire near/mid field (view_w < 1.3x ref, which in the p60
    // device-inset repro is bands 3..near, the abundant-shadow region incl. the mid peak) at full
    // strength; end=2.6 fully fades by the far frustum edge so the far quarter — where the live
    // steep-pitch cut-off appears — always reads as a graceful taper, never a hard horizontal line.
    // Empirically calibrated against the Phase-1 WITH-insets p60 tall repro (near field byte-identical
    // to no-fade; far bands 0..2 taper smoothly). end<=start or end<=0 disables.
    const float startMul = envFloat("MLN_SHADOW_DEPTH_FADE_START", 1.3f);
    const float endMul = envFloat("MLN_SHADOW_DEPTH_FADE_END", 2.6f);
    if (pitchDeg < fadePitch || endMul <= startMul || endMul <= 0.0f) {
        return b; // disabled (end stays 0)
    }

    // world-per-pixel at the geometric screen center (matches the radius-cap derivation).
    const Size sz = state.getSize();
    const vec3 cWorld = screenPixelToWorld(state, focalZoom, 0.5 * sz.width, 0.5 * sz.height);
    const vec3 cWorldDx = screenPixelToWorld(state, focalZoom, 0.5 * sz.width + 1.0, 0.5 * sz.height);
    const double worldPerPixel = std::hypot(cWorldDx[0] - cWorld[0], cWorldDx[1] - cWorld[1]);
    const double refViewW = state.getCameraToCenterDistance() * worldPerPixel;
    if (!std::isfinite(refViewW) || refViewW <= 0.0) {
        return b;
    }
    b.start = static_cast<float>(refViewW) * startMul;
    b.end = static_cast<float>(refViewW) * endMul;
    return b;
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

    // Tighten the footprint to a bounded box around the map center. Derive the focal point
    // through TransformState::matrixFor(), the same tile-local-to-world path used by the
    // per-tile light_matrix, but choose the zoom from camera state rather than layer-group
    // drawable order. The caster, building receiver, and ground receiver may contain different
    // drawable sets while tiles stream in; the light frustum must not depend on those sets.
    const uint8_t focalZoom = cameraFocalZoom(state);
    const LatLng cameraCenter = state.getLatLng(LatLng::Unwrapped);
    const Point<double> projectedCenter = Projection::project(cameraCenter, state.getScale());
    const vec3 focalCenter = centerPixelToWorld(state, focalZoom);
    const double maxHeightRaw = envFloat("MLN_SHADOW_MAX_HEIGHT", 200.0f);
    const double maxHeightWorld = maxHeightRaw * shadowWorldZScale();
    const Point<double> footprintCenter = heightCompensatedFootprintCenter(focalCenter, sunDir, maxHeightWorld);
    const double cx = footprintCenter.x;
    const double cy = footprintCenter.y;

    // Coverage radius: size the light frustum so the whole on-screen map is shadow-mapped. A fixed
    // radius (or one fit only to a few viewport corners, which under-shoots at pitch where the top
    // samples land near the horizon) leaves the far/top of the view with no shadow data. Keep the
    // box centered on the aligned focal point (UV stays centered); grow only the half-extent.
    // Clamp the top so the shadow-map texel size stays usable. MLN_SHADOW_RADIUS forces it (debug).
    //
    // Critical: derive the frustum from camera state only, not from the per-layer-group loaded tiles.
    // A tile term computed per group can differ between caster and receivers, so the caster would
    // render depth into one frustum while the ground samples another. A denser screen grid keeps
    // coverage robust under pitch.
    const double radiusMax = static_cast<double>(envFloat("MLN_SHADOW_RADIUS_MAX", 4000.0f));
    const Size sz = state.getSize();
    double screenExtent = 0.0;
    {
        const double fr[6] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
        for (double fx : fr) {
            for (double fy : fr) {
                const vec3 w = screenPixelToWorld(state, focalZoom, fx * sz.width, fy * sz.height);
                const double d = std::max(std::abs(w[0] - cx), std::abs(w[1] - cy));
                if (std::isfinite(d) && d > 0.0) {
                    screenExtent = std::max(screenExtent, std::min(d, radiusMax));
                }
            }
        }
    }
    // Distance cap (Mapbox-v3 technique): bound the shadow volume to a multiple of the
    // camera-to-center distance instead of letting the screen grid reach the horizon. At
    // steep pitch the top screen rows graze the horizon and screenExtent balloons (~630 at
    // p35 -> ~2043 at p60), packing the loaded casters into a small shadow-map fraction and
    // extending the frustum past the loaded tiles -> the far/top of the view has no caster
    // depth -> a hard cut-off line. The cap keeps casters texel-dense. It only bites at steep
    // pitch (where screenExtent >> cap); at low pitch the cap is large so the near-field
    // radius is unchanged (no near-field regression). Convert cameraToCenterDistance (screen
    // px) to world units via the local world-per-pixel scale at the focal center.
    const vec3 cWorld = screenPixelToWorld(state, focalZoom, 0.5 * sz.width, 0.5 * sz.height);
    const vec3 cWorldDx = screenPixelToWorld(state, focalZoom, 0.5 * sz.width + 1.0, 0.5 * sz.height);
    const double worldPerPixel = std::hypot(cWorldDx[0] - cWorld[0], cWorldDx[1] - cWorld[1]);
    // ~1.0 * cameraToCenterDistance (world units) is the sweet spot: it leaves the low-pitch
    // near-field radius (p0~537, p35~630) untouched (cap ~889 > those) and caps the steep-pitch
    // balloon (p55~1411, p60~2043) back to a texel-dense ~889. Matches the empirically-verified
    // good-coverage radius and Mapbox-v3's cameraToCenter-bounded shadow volume.
    const double distFactor = static_cast<double>(envFloat("MLN_SHADOW_DIST_FACTOR", 1.0f));
    const double distCapWorld = state.getCameraToCenterDistance() * distFactor * worldPerPixel;
    // distCap = end of the shadow volume in world units. (Avoid the name `far`: it is a
    // legacy reserved macro in some toolchains.)
    const double distCap = std::isfinite(distCapWorld) && distCapWorld > 0.0
                               ? std::min(distCapWorld, radiusMax)
                               : radiusMax;
    const double coverRadius = std::min(screenExtent, distCap);
    const double radius = std::getenv("MLN_SHADOW_RADIUS")
                              ? static_cast<double>(envFloat("MLN_SHADOW_RADIUS", 700.0f))
                              : std::clamp(coverRadius,
                                           static_cast<double>(envFloat("MLN_SHADOW_RADIUS_MIN", 350.0f)),
                                           radiusMax);

    if (std::getenv("MLN_SHADOW_DBG")) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "MLN_SHADOW_DBG pitch=%.1f zoom=%.2f focalZoom=%u size=%dx%d "
                      "focal=(%.1f,%.1f) projCenter=(%.1f,%.1f) footCenter=(%.1f,%.1f) "
                      "focalVsProj=(%.1f,%.1f) screenExtent=%.1f wpp=%.3f distCap=%.1f radius=%.1f",
                      util::rad2deg(state.getPitch()), state.getZoom(),
                      focalZoom, sz.width, sz.height,
                      focalCenter[0], focalCenter[1], projectedCenter.x, projectedCenter.y,
                      footprintCenter.x, footprintCenter.y,
                      focalCenter[0] - projectedCenter.x, focalCenter[1] - projectedCenter.y,
                      screenExtent, worldPerPixel, distCap, radius);
        Log::Warning(Event::General, buf);
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
        // Fade the cast-shadow strength out as the camera flattens (top-down has no 3D depth
        // cue; flat shadows clutter the map). Face shading uses light_intensity, not this, so
        // the buildings keep their directional shading at any pitch.
        .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.5f) * pitchShadowFade(state),
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

    const DepthFadeBounds depthFade = computeDepthFadeBounds(state, cameraFocalZoom(state));

    const GroundShadowPropsUBO propsUBO = {.shadow_color = Color::black(),
                                           // Fade ground cast shadows out at flat/top-down pitch
                                           // (they have no 3D depth cue there and clutter the map).
                                           .shadow_intensity = envFloat("MLN_SHADOW_INTENSITY", 0.5f) *
                                                               pitchShadowFade(state),
                                           .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
                                           .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0015f),
                                           // UV-radial frustum-rim fade. With the primary view-depth
                                           // fade now carrying the near→far softening, only trim the
                                           // very edge (default 0.92); MLN_SHADOW_FADE_START=1.0 off.
                                           .shadow_fade_start = envFloat("MLN_SHADOW_FADE_START", 0.92f),
                                           // Primary near→far view-depth fade (camera distance), in
                                           // clip-space w. Gated to steep pitch; 0/0 = disabled.
                                           .depth_fade_start = depthFade.start,
                                           .depth_fade_end = depthFade.end,
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
