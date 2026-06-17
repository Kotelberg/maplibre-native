#include <mbgl/renderer/shadows/shadow_tweakers.hpp>

#include <mbgl/renderer/shadows/shadow_sun.hpp>
#include <mbgl/renderer/shadows/shadow_frustum.hpp>
#include <mbgl/renderer/shadows/shadow_pass.hpp>
#include <mbgl/shaders/shadow_depth_ubo.hpp>
#include <mbgl/shaders/fill_extrusion_shadow_ubo.hpp>
#include <mbgl/shaders/fill_extrusion_layer_ubo.hpp>
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

// Shadows are a 3D-building effect: fade their strength in with the building-height zoom ramp so
// near-flat buildings (height interpolated to ~0 at low zoom) don't cast footprint-shaped shadow
// blobs on the 2D map. Defaults match HataHub's fill-extrusion-height interpolate(zoom,15,0,15.05,H):
// 0 below z15, ramping to full by z15.05 (the buildings "pop up" almost immediately past z15 rather
// than growing over a whole zoom level). Keep this HI in lock-step with the style's upper height stop —
// if they diverge, shadows lag or lead the buildings. Env-tunable for other styles.
float shadowHeightFade(float zoom) {
    const float lo = envFloat("MLN_SHADOW_GROW_ZOOM_LO", 15.0f);
    const float hi = envFloat("MLN_SHADOW_GROW_ZOOM_HI", 15.05f);
    if (hi <= lo) {
        return 1.0f;
    }
    const float t = (zoom - lo) / (hi - lo);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// World-units-per-meter for the building HEIGHT axis. The fill-extrusion height vertex is in
// METERS, not mercator world-pixels: the visible FE perspective path converts it via
// Camera::getWorldToCamera()'s "Post-multiply z" by pixelsPerMeter (= worldSize / (cos(lat) *
// 2π * EARTH_RADIUS_M)), which GROWS with zoom (worldSize = scale·512). The shadow caster +
// receiver build their light matrix from matrixFor() (z-scale 1) and so previously fed the raw
// meters straight in — leaving the caster height fixed in world-px while the x/y footprint scaled
// with zoom. Result: the cast shadow shrank relative to the building as you zoomed in (shadow
// length should be height·tan(sun) in WORLD space, independent of camera zoom). Mirror the camera
// here so the caster height tracks the footprint at every zoom. MLN_SHADOW_ZSCALE stays as a
// multiplicative dialing knob on top.
double pixelsPerMeter(const TransformState& state) {
    const double worldSize = Projection::worldSize(state.getScale());
    const double latitude = state.getLatLng(LatLng::Unwrapped).latitude();
    return worldSize / (std::cos(util::deg2rad(latitude)) * util::M2PI * util::EARTH_RADIUS_M);
}

double tileWorldZScale(const TransformState& state, const OverscaledTileID&) {
    return pixelsPerMeter(state) * envFloat("MLN_SHADOW_ZSCALE", 1.0f);
}

void matrixForLightTileWorld(mat4& tileWorld, const TransformState& state, const OverscaledTileID& tileID) {
    state.matrixFor(tileWorld, tileID.toUnwrapped());
    const double zScale = tileWorldZScale(state, tileID);
    // matrixFor gives x/y in world-pixels (scale with zoom) but leaves z at unit scale; the FE
    // height vertex is in METERS, so scale z by world-pixels-per-meter (#2) to put the caster
    // height in the same units as its footprint — keeping the cast-shadow length world-fixed
    // (independent of camera zoom) instead of shrinking as you zoom in.
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

// Screen-pixel offset of the PADDED look-at from the geometric screen center, from the camera's
// edge insets (mirrors the private TransformState::getCenterOffset()). Zero with no insets.
ScreenCoordinate paddedCenterOffset(const TransformState& state) {
    const EdgeInsets ins = state.getEdgeInsets();
    return {0.5 * (ins.left() - ins.right()), 0.5 * (ins.top() - ins.bottom())};
}

vec3 centerPixelToWorld(const TransformState& state, uint8_t tileZoom) {
    const Size size = state.getSize();
    // Center on the PADDED look-at, not the geometric screen center: under CAMERA_PADDING / edge
    // insets (e.g. HataHub's bottom-sheet {t88,r24,b130,l24} → offset≈(0,-21)) the visible map
    // center is offset up-screen, so the light frustum should center there for optimal coverage.
    // With no insets the offset is (0,0) → identical to the geometric center (no-padding path
    // unchanged). Offsetting the screen pixel then unprojecting via the same path is unit-correct
    // (NOT the earlier projectedCenter*tileSize_D approach, which was a 512x regression).
    const ScreenCoordinate off = paddedCenterOffset(state);
    return screenPixelToWorld(state, tileZoom, 0.5 * size.width + off.x, 0.5 * size.height + off.y);
}

uint8_t cameraFocalZoom(const TransformState& state) {
    return std::min(state.getIntegerZoom(), util::DEFAULT_MAX_ZOOM);
}

const std::vector<mat4>& worldToLightClipForFrame(ShadowFrustumState& frustumState,
                                                  const PaintParameters& parameters,
                                                  uint32_t mapSize) {
    const uint32_t count = shadowCascadeCount();
    if (!frustumState.valid || frustumState.frameCount != parameters.frameCount ||
        frustumState.mapSize != mapSize || frustumState.cascadeCount != count) {
        frustumState.cascades = computeWorldToLightClipCascades(parameters, mapSize, count, shadowCascadeSplit());
        frustumState.cascadeCount = count;
        frustumState.frameCount = parameters.frameCount;
        frustumState.mapSize = mapSize;
        frustumState.valid = true;
    }
    return frustumState.cascades;
}

} // namespace

mat4 computeWorldToLightClip(const PaintParameters& parameters, uint32_t mapSize) {
    const auto& state = parameters.state;
    const vec3 sunDir = ShadowSun::direction(parameters.evaluatedLight.get<LightPosition>(),
                                             parameters.evaluatedLight.get<LightAnchor>(),
                                             static_cast<float>(state.getBearing()));
    return computeWorldToLightClip(state, sunDir, mapSize);
}

std::vector<mat4> computeWorldToLightClipCascades(const PaintParameters& parameters, uint32_t mapSize,
                                                  uint32_t cascadeCount, float split) {
    const auto& state = parameters.state;
    const vec3 sunDir = ShadowSun::direction(parameters.evaluatedLight.get<LightPosition>(),
                                             parameters.evaluatedLight.get<LightAnchor>(),
                                             static_cast<float>(state.getBearing()));
    return computeWorldToLightClipCascades(state, sunDir, mapSize, cascadeCount, split);
}

std::vector<mat4> computeWorldToLightClipCascades(const TransformState& state, const vec3& sunDir,
                                                  uint32_t mapSize, uint32_t cascadeCount, float split) {
    // WORLD-ANCHORED light frustum, sized to COVER the visible ground but BEARING-INVARIANT so the
    // shadows don't move when the camera rotates (the user's core requirement).
    //
    // CASCADES: this produces `cascadeCount` CONCENTRIC frustums that all share the SAME focalCenter
    // and the SAME sun-ground axes, differing ONLY in radius (cascade count-1 = the full far radius
    // computed below; nearer cascades shrink by `split` per step). Because the only per-cascade
    // parameter is a scalar radius — and the focal center + sun axes are bearing-invariant — every
    // cascade stays put under camera rotation, just like the single map. A near cascade packs the
    // same 2048 texels into a smaller world square → higher density on near buildings (fixes the
    // coarse/zoom-growing wall shadows, artifact A) while the far cascade keeps the pitched-horizon
    // coverage (artifact B). cascadeCount==1 reproduces the legacy single-map fit byte-for-byte.
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
    // Z-range of the light frustum, in WORLD-PIXELS. MLN_SHADOW_MAX_HEIGHT is a height in METERS
    // (default 200m), so convert with the same per-zoom pixelsPerMeter the caster height uses —
    // otherwise a tall building's scaled-up caster roof would punch out of a fixed-world-px frustum
    // top at high zoom and get Z-clipped (→ short/missing shadow). Tracks zoom like the casters.
    const double maxHeightWorld = envFloat("MLN_SHADOW_MAX_HEIGHT", 200.0f) *
                                  pixelsPerMeter(state) * envFloat("MLN_SHADOW_ZSCALE", 1.0f);

    const Size sz = state.getSize();
    const double screenExtent = 0.5 * std::hypot(static_cast<double>(sz.width), static_cast<double>(sz.height));
    // Floor: at flat pitch the visible field is ~the screen; keep at least this much coverage. 1.8
    // also pushes the pitched far-cutoff out (#3). The texel-density cost of this coverage is paid by
    // the 2048 shadow map (MLN_SHADOW_MAP_SIZE): 2*radius/mapSize = ~0.76 world-px/texel here, 2x
    // finer than the old 1024 map — which removes the staircased edges (#17/C/A) without trading away
    // coverage (#3/B). (A still-finer near field + wider pitched coverage at once would need cascaded
    // shadow maps; out of scope.)
    const double minRadius = static_cast<double>(envFloat("MLN_SHADOW_MIN_RADIUS", 1.8f)) * screenExtent;
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
        // Walk down the PADDED centerline (matches the padded focalCenter) so the forward-reach
        // distances are measured from the same look-at the frustum is centered on.
        const double colX = 0.5 * sz.width + paddedCenterOffset(state).x;
        const double fy[6] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
        for (double f : fy) {
            const vec3 w = screenPixelToWorld(state, focalZoom, colX, f * sz.height);
            const double d = std::hypot(w[0] - focalCenter[0], w[1] - focalCenter[1]);
            if (std::isfinite(d)) {
                reach = std::max(reach, std::min(d, maxDist));
            }
        }
    }
    const double farRadius = std::min(std::max(minRadius, reach * cornerFactor), maxDist);

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
    // (These axes + focalCenter are shared by every cascade — only the radius shrinks per cascade.)
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

#ifndef NDEBUG
    // DEV-ONLY frustum trace (compiled out of release/opt; env-gated within debug builds).
    if (std::getenv("MLN_SHADOW_DBG")) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "MLN_SHADOW_DBG pitch=%.1f zoom=%.2f focalZoom=%u size=%dx%d focal=(%.1f,%.1f) "
                      "farRadius=%.1f screenExtent=%.1f maxDist=%.0f cascades=%u",
                      util::rad2deg(state.getPitch()), state.getZoom(), focalZoom, sz.width, sz.height,
                      focalCenter[0], focalCenter[1], farRadius, screenExtent, maxDist, cascadeCount);
        Log::Warning(Event::General, buf);
    }
#endif

    // Build one fitted world->light-clip matrix for a given square half-side `radius`. The footprint,
    // height-expand and texel-snapped ortho fit are IDENTICAL to the legacy single-map path; only the
    // radius varies between cascades. For cascadeCount==1 the single call uses farRadius → the result
    // is byte-for-byte the pre-cascade matrix.
    const auto fitForRadius = [&](double radius) -> mat4 {
        const std::vector<vec3> footprint = {
            {focalCenter[0] - radius * rgx - radius * sgx, focalCenter[1] - radius * rgy - radius * sgy, 0.0},
            {focalCenter[0] + radius * rgx - radius * sgx, focalCenter[1] + radius * rgy - radius * sgy, 0.0},
            {focalCenter[0] - radius * rgx + radius * sgx, focalCenter[1] - radius * rgy + radius * sgy, 0.0},
            {focalCenter[0] + radius * rgx + radius * sgx, focalCenter[1] + radius * rgy + radius * sgy, 0.0},
        };
        const std::vector<vec3> pts = ShadowFrustum::heightExpand(footprint, maxHeightWorld);
        // Texel-snap the light frustum so the shadow-map sampling grid is stable in world space as the
        // camera pans/rotates. The symmetric radius keeps the box bearing-invariant; the snap then
        // anchors the grid so shadows don't crawl. (Pitch changes resize the box — a brief, rare
        // transient.) Requires a world-fixed sun (map anchor); see the style's light.anchor.
        return ShadowFrustum::fit(sunDir, pts, mapSize, /*texelSnapEnabled=*/true);
    };

    const uint32_t count = cascadeCount < 1u ? 1u : cascadeCount;
    std::vector<mat4> cascades;
    cascades.reserve(count);
    for (uint32_t c = 0; c < count; ++c) {
        // Concentric radii: the LAST cascade (c == count-1) is the full far radius; each nearer
        // cascade is `split` times the next one (geometric), floored at minRadius so the tightest
        // cascade never collapses below ~the visible screen. count==1 ⇒ the single radius == farRadius.
        double radius = farRadius;
        if (count > 1) {
            // factor == 1 for the last cascade (split^0) → the full far radius; < 1 for nearer ones.
            // NO minRadius floor here: minRadius keeps the FAR cascade covering the whole screen, but
            // the near cascades are DELIBERATELY tighter than the screen — that is the resolution win.
            // Fragments outside a near cascade's box fall back to the far cascade (containment select).
            const double factor = std::pow(static_cast<double>(split), static_cast<double>(count - 1 - c));
            radius = farRadius * factor;
        }
        cascades.push_back(fitForRadius(radius));
    }
    return cascades;
}

mat4 computeWorldToLightClip(const TransformState& state, const vec3& sunDir, uint32_t mapSize) {
    // Legacy single-map entry point: one cascade at the full far radius (byte-identical to the
    // pre-cascade fit). Retained for callers/tests that want exactly one frustum.
    return computeWorldToLightClipCascades(state, sunDir, mapSize, 1u, shadowCascadeSplit()).front();
}

void ShadowDepthTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const auto& state = parameters.state;
    const std::vector<mat4>& cascades = worldToLightClipForFrame(*frustumState, parameters, mapSize);
    // This caster pass renders into cascade `cascadeIndex`'s shadow map, so it must use that
    // cascade's frustum (clamped defensively in case the active count shrank).
    const uint32_t idx = std::min(cascadeIndex, static_cast<uint32_t>(cascades.size()) - 1u);
    const mat4& worldToLightClip = cascades[idx];

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

    const std::vector<mat4>& cascades = worldToLightClipForFrame(*frustumState, parameters, mapSize);
    const auto cascadeCount = static_cast<uint32_t>(cascades.size());

    // Per-layer props (shared across drawables).
    const auto lightColor = FillExtrusionBucket::lightColor(parameters.evaluatedLight);
    const auto lightPos = FillExtrusionBucket::lightPosition(parameters.evaluatedLight, state);
    const float base = evaluated.get<FillExtrusionBase>().constantOr(0.0f);
    const auto zoom = static_cast<float>(state.getZoom());
    // Fade shadow strength in with the building-height zoom ramp (#9): no footprint blobs at low
    // zoom where buildings are flat.
    // Clamp the style/env shadow-intensity to [0,1] (#32: author values can't over-darken or — now
    // that the debug branches are gone — reach any out-of-range path), then fade by the height ramp.
    const float baseIntensity = std::clamp(envFloat("MLN_SHADOW_INTENSITY",
                                                    parameters.evaluatedLight.get<LightShadowIntensity>()),
                                           0.0f, 1.0f) *
                                shadowHeightFade(zoom);
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
        .shadow_intensity = baseIntensity,
        .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
        // BUILDING receiver bias stays 0.0015 (the shipped value): buildings CAST and so self-shadow —
        // 0 here reintroduces horizontal roof acne stripes under a grazing sun (verified). The GROUND
        // receiver (flat, never casts) keeps bias 0 so wall cast shadows reattach to the base.
        .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0015f),
        // Slope-scaled bias (×(1−n·L)): large on away/grazing faces (which the directional lighting
        // already darkens) so the building never self-shadows them into acne stripes (#8), ~0 on
        // sun-facing faces so a neighbour's cast shadow still lands. Headless-verified: kills the
        // wall/roof acne (HF energy 16/42→0) while inter-building + ground shadows survive.
        .shadow_slope_bias = envFloat("MLN_SHADOW_SLOPE_BIAS", 0.05f)};
    // On the INSTANCED path the building WALLS are a separate drawable drawn by the PLAIN
    // FillExtrusionInstancedShader (the receiver only draws the roof; walls don't show cast shadows —
    // wallness suppresses them — so they intentionally stay on the plain shader). That shader reads the
    // STANDARD FillExtrusionPropsUBO + FillExtrusionDrawableUBO, normally provided by
    // FillExtrusionLayerTweaker, which THIS tweaker replaces whenever shadows are on. Without them the
    // walls read all-zero props (opacity 0 → invisible). Build the regular props once here; HOW the walls
    // receive props + the drawable UBO differs per backend (below).
    const auto& crossfade = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).crossfade;
    const FillExtrusionPropsUBO regularPropsUBO = {
        .color = evaluated.get<FillExtrusionColor>().constantOr(Color::black()),
        .light_color = lightColor,
        .pad1 = 0,
        .light_position = lightPos,
        .base = base,
        .height = evaluated.get<FillExtrusionHeight>().constantOr(0.0f),
        .light_intensity = FillExtrusionBucket::lightIntensity(parameters.evaluatedLight),
        .vertical_gradient = evaluated.get<FillExtrusionVerticalGradient>() ? 1.0f : 0.0f,
        .opacity = evaluated.get<FillExtrusionOpacity>(),
        .fade = crossfade.t,
        .from_scale = crossfade.fromScale,
        .to_scale = crossfade.toScale,
        .pad2 = 0};

#if !MLN_RENDER_BACKEND_VULKAN
    // Metal/GL: the shadow props is layer-constant and reaches the receiver via a flat buffer index
    // (Metal) or a named UBO block (GL), so upload it ONCE per layer (cheap; the shipped behavior). The
    // walls' regular props + drawable UBO are bound PER-DRAWABLE in the visitor below — NOT at the layer
    // level: the shadow RECEIVER's vertex buffers begin at buffer index `fillExtrusionShadowUBOCount`,
    // which aliases idFillExtrusionPropsUBO, so the roof (drawn first) binds its pos buffer over a
    // layer-bound props; only re-binding in the wall's own draw (after the roof) survives.
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    layerUniforms.createOrUpdate(idFillExtrusionShadowPropsUBO, &propsUBO, context);
#endif

#if MLN_RENDER_BACKEND_VULKAN
    // Vulkan: the receiver declares its shadow props at the DRAWABLE descriptor set, so it's uploaded
    // per-drawable in the visitor (a layer-group write would land in an unbound slot — idFillExtrusion-
    // ShadowPropsUBO is a drawable-range id — and the receiver would read all-zero -> invisible). The
    // visible WALLS, by contrast, read the regular CONSOLIDATED FillExtrusionDrawableUBO vector (LAYER
    // set, indexed by ubo_index) + the layer FillExtrusionPropsUBO — the same model FillExtrusionLayer-
    // Tweaker uses. Per-drawable writes can't reach a LAYER-set binding on Vulkan, so rebuild the wall
    // drawable-UBO vector across the visitor and bind it + the layer props after the visitor.
    std::vector<FillExtrusionDrawableUBO> wallDrawableUBOs;
#endif

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
        // tile-local -> light clip for each cascade (kMaxShadowCascades slots; unused slots replicate
        // cascade 0 so the UBO is fully initialized — the receiver only reads the first cascade_count).
        std::array<std::array<float, 16>, 4> lightMatrices{};
        for (uint32_t c = 0; c < 4; ++c) {
            mat4 lm;
            matrix::multiply(lm, cascades[std::min(c, cascadeCount - 1u)], tileWorld);
            lightMatrices[c] = util::cast<float>(lm);
        }

        const float baseT = std::get<0>(binders->get<FillExtrusionBase>()->interpolationFactor(zoom));
        const float heightT = std::get<0>(binders->get<FillExtrusionHeight>()->interpolationFactor(zoom));
        const float colorT = std::get<0>(binders->get<FillExtrusionColor>()->interpolationFactor(zoom));

#if MLN_RENDER_BACKEND_VULKAN
        if (drawable.getInstanceAttributes()) {
            // INSTANCED WALL drawable (plain FillExtrusionInstancedShader): collect its REGULAR
            // FillExtrusionDrawableUBO into the consolidated wall vector (bound at the LAYER level after
            // the visitor) and index into it. The walls read the regular drawable UBO + layer props, NOT
            // the shadow UBOs the receiver roof uses.
            drawable.setUBOIndex(static_cast<std::uint32_t>(wallDrawableUBOs.size()));
            wallDrawableUBOs.push_back(FillExtrusionDrawableUBO{
                .matrix = util::cast<float>(matrix),
                .pixel_coord_upper = {0, 0},
                .pixel_coord_lower = {0, 0},
                .height_factor = 0,
                .tile_ratio = 0,
                .base_t = baseT,
                .height_t = heightT,
                .color_t = colorT,
                .pattern_from_t = 0,
                .pattern_to_t = 0,
                .pad1 = 0});
            return;
        }
#endif

#if !MLN_RENDER_BACKEND_VULKAN
        if (drawable.getInstanceAttributes()) {
            // INSTANCED WALL drawable (plain FillExtrusionInstancedShader): it reads the REGULAR
            // FillExtrusionDrawableUBO (matrix + base_t/height_t/color_t) at idFillExtrusionDrawableUBO,
            // NOT the shadow drawable UBO. That slot ALIASES idFillExtrusionShadowDrawableUBO, so write
            // the regular layout here per-drawable (the receiver roof, below, writes the shadow layout to
            // the same slot — per-drawable, so they don't collide). uboIndex 0: single per-drawable UBO.
            const FillExtrusionDrawableUBO wallUBO = {
                .matrix = util::cast<float>(matrix),
                .pixel_coord_upper = {0, 0},
                .pixel_coord_lower = {0, 0},
                .height_factor = 0,
                .tile_ratio = 0,
                .base_t = baseT,
                .height_t = heightT,
                .color_t = colorT,
                .pattern_from_t = 0,
                .pattern_to_t = 0,
                .pad1 = 0};
            drawable.setUBOIndex(0);
            drawable.mutableUniformBuffers().createOrUpdate(idFillExtrusionDrawableUBO, &wallUBO, context);
            // PER-DRAWABLE props (NOT layer-level): the shadow RECEIVER roof draws before the wall and
            // binds its own pos vertex buffer at buffer index `fillExtrusionShadowUBOCount` (5), which
            // ALIASES idFillExtrusionPropsUBO (5) — clobbering the layer-bound props. Re-binding props
            // in the wall's own per-drawable set restores it for the wall's draw (which happens after the
            // roof's). Safe only on walls: the roof's slot 5 IS its vertex buffer, so writing props there
            // would corrupt its geometry (the wall binds pos at slot 6, so slot 5 is free for props).
            drawable.mutableUniformBuffers().createOrUpdate(idFillExtrusionPropsUBO, &regularPropsUBO, context);
            return;
        }
#endif

        const FillExtrusionShadowDrawableUBO ubo = {
            .matrix = util::cast<float>(matrix),
            .light_matrix = lightMatrices,
            .base_t = baseT,
            .height_t = heightT,
            .color_t = colorT,
            .cascade_count = static_cast<std::int32_t>(cascadeCount)};
        drawable.mutableUniformBuffers().createOrUpdate(idFillExtrusionShadowDrawableUBO, &ubo, context);
#if MLN_RENDER_BACKEND_VULKAN
        // Vulkan-only: props lives in the drawable descriptor set (see the comment above the visitor).
        drawable.mutableUniformBuffers().createOrUpdate(idFillExtrusionShadowPropsUBO, &propsUBO, context);
#endif
    });

#if MLN_RENDER_BACKEND_VULKAN
    // Bind the consolidated wall drawable-UBO vector + the layer props so the visible instanced walls
    // (plain FillExtrusionInstancedShader) render — mirroring FillExtrusionLayerTweaker, which this
    // tweaker replaces when shadows are on. The roof receivers use the separate per-drawable shadow UBOs.
    if (!wallDrawableUBOs.empty()) {
        auto& layerUniforms = layerGroup.mutableUniformBuffers();
        const std::size_t wallVectorSize = sizeof(FillExtrusionDrawableUBO) * wallDrawableUBOs.size();
        layerUniforms.set(idFillExtrusionDrawableUBO,
                          context.createUniformBuffer(wallDrawableUBOs.data(), wallVectorSize, false, true));
        layerUniforms.createOrUpdate(idFillExtrusionPropsUBO, &regularPropsUBO, context);
    }
#endif
}

void GroundShadowTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }
    auto& context = parameters.context;
    const auto& state = parameters.state;
    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;

    const std::vector<mat4>& cascades = worldToLightClipForFrame(*frustumState, parameters, mapSize);
    const auto cascadeCount = static_cast<uint32_t>(cascades.size());

    const float groundIntensity = std::clamp(envFloat("MLN_SHADOW_INTENSITY",
                                                      parameters.evaluatedLight.get<LightShadowIntensity>()),
                                             0.0f, 1.0f) *
                                  shadowHeightFade(static_cast<float>(state.getZoom()));
    const GroundShadowPropsUBO propsUBO = {.shadow_color = Color::black(),
                                           // World-anchored: constant strength at every pitch (see
                                           // FillExtrusionShadowTweaker), faded in with the building
                                           // height zoom ramp (#9) so flat low-zoom footprints cast none.
                                           .shadow_intensity = groundIntensity,
                                           .shadow_texel_size = 1.0f / static_cast<float>(mapSize),
                                           .shadow_bias = envFloat("MLN_SHADOW_BIAS", 0.0f),
                                           // UV-radial frustum-rim fade: softens the hard edge of the
                                           // bounded light frustum (a fixed WORLD radius around the
                                           // look-at point) so coverage tapers out in world space, not
                                           // by pitch. Default 0.75 (#10: wide taper so far shadows fade
                                           // in gradually on pan, not a hard cutoff). =1.0 to disable.
                                           .shadow_fade_start = envFloat("MLN_SHADOW_FADE_START", 0.75f),
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
        // tile-local -> light clip per cascade (unused slots replicate cascade 0; the receiver reads
        // only the first cascade_count, plus the last for the rim fade).
        std::array<std::array<float, 16>, 4> lightMatrices{};
        for (uint32_t c = 0; c < 4; ++c) {
            mat4 lm;
            matrix::multiply(lm, cascades[std::min(c, cascadeCount - 1u)], tileWorld);
            lightMatrices[c] = util::cast<float>(lm);
        }

        const GroundShadowDrawableUBO ubo = {.matrix = util::cast<float>(matrix),
                                             .light_matrix = lightMatrices,
                                             .cascade_count = static_cast<std::int32_t>(cascadeCount),
                                             .pad0 = 0.0f,
                                             .pad1 = 0.0f,
                                             .pad2 = 0.0f};
        // Props set per-drawable (NOT on the shared group) so they don't collide with the
        // FillExtrusionShadow group props at the aliased slot.
        auto& uniforms = drawable.mutableUniformBuffers();
        uniforms.createOrUpdate(idGroundShadowDrawableUBO, &ubo, context);
        uniforms.createOrUpdate(idGroundShadowPropsUBO, &propsUBO, context);
    });
}

} // namespace mbgl
