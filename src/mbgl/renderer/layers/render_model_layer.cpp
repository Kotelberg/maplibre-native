#include <mbgl/renderer/layers/render_model_layer.hpp>

#include <mbgl/gfx/drawable.hpp>
#include <mbgl/renderer/model/placeholder_mesh.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/style/sources/geojson_source_impl.hpp>
#include <mbgl/tile/geojson_tile_data.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/util/projection.hpp>

#include <cmath>

namespace mbgl {

using namespace style;

namespace {

inline const ModelLayer::Impl& impl(const Immutable<style::Layer::Impl>& impl_) {
    assert(impl_->getTypeInfo() == ModelLayer::Impl::staticTypeInfo());
    return static_cast<const ModelLayer::Impl&>(*impl_);
}

// Evaluate a possibly data-driven property against one feature.
template <typename T>
T evaluateFor(const PropertyValue<T>& value, const GeometryTileFeature& feature, const T& defaultValue) {
    if (value.isUndefined()) {
        return defaultValue;
    }
    if (value.isConstant()) {
        return value.asConstant();
    }
    return value.asExpression().evaluate(feature, defaultValue);
}

// Latitude of a mercator world fraction y in [0, 1].
inline double latitudeFromMercatorFraction(double y) {
    return util::rad2deg(2.0 * std::atan(std::exp(M_PI * (1.0 - 2.0 * y))) - M_PI_2);
}

} // namespace

RenderModelLayer::RenderModelLayer(Immutable<style::ModelLayer::Impl> _impl)
    : RenderLayer(makeMutable<ModelLayerProperties>(std::move(_impl))) {}

RenderModelLayer::~RenderModelLayer() = default;

void RenderModelLayer::evaluate(const PropertyEvaluationParameters&) {
    passes = RenderPass::Translucent;
    evaluatedProperties = makeMutable<ModelLayerProperties>(staticImmutableCast<ModelLayer::Impl>(baseImpl));
}

bool RenderModelLayer::hasTransition() const {
    return false;
}

bool RenderModelLayer::hasCrossfade() const {
    return false;
}

void RenderModelLayer::prepare(const LayerPrepareParameters&) {}

void RenderModelLayer::update(gfx::ShaderRegistry& shaders,
                              gfx::Context& context,
                              const TransformState& state,
                              const std::shared_ptr<UpdateParameters>& updateParameters,
                              const RenderTree& renderTree,
                              UniqueChangeRequestVec& changes) {
    // Locate this layer's GeoJSON source.
    std::shared_ptr<style::GeoJSONData> data;
    for (const auto& source : *updateParameters->sources) {
        if (source->id == baseImpl->source && source->type == SourceType::GeoJSON) {
            data = static_cast<const style::GeoJSONSource::Impl&>(*source).getData().lock();
            break;
        }
    }
    if (!data) {
        return;
    }

    // Fetch the tile containing the camera center at the current integer zoom:
    // z0 int16 tile coordinates quantize to ~5 km cells, far too coarse for
    // placement. M3a limitation: features outside the center tile (+buffer)
    // are not placed — the real tile lifecycle lands in M5.
    const auto z = static_cast<uint8_t>(std::clamp(static_cast<int>(state.getZoom()), 0, 18));
    const double tiles = static_cast<double>(1u << z);
    const double worldSizeNow = Projection::worldSize(state.getScale());
    const Point<double> centerPx = Projection::project(state.getLatLng(), state.getScale());
    const auto tx = static_cast<uint32_t>(
        std::clamp(std::floor(centerPx.x / worldSizeNow * tiles), 0.0, tiles - 1));
    const auto ty = static_cast<uint32_t>(
        std::clamp(std::floor(centerPx.y / worldSizeNow * tiles), 0.0, tiles - 1));
    const CanonicalTileID tileId{z, tx, ty};

    GeoJSONData::TileFeatures features;
    data->getTile(tileId, [&](GeoJSONData::TileFeatures f) { features = std::move(f); },
                  /*runSynchronously=*/true);

    // Rebuild drawables only when the style impl, source data, or tile changed.
    const bool changed = lastImpl != baseImpl.get() || lastData != data.get() ||
                         lastFeatureCount != features.size() || !(lastTile == tileId);
    if (!changed) {
        return;
    }
    lastImpl = baseImpl.get();
    lastData = data.get();
    lastFeatureCount = features.size();
    lastTile = tileId;

    CustomDrawableLayerHost::Interface interface(
        *this, layerGroup, shaders, context, state, updateParameters, renderTree, changes);

    for (const auto& id : drawableIds) {
        interface.removeDrawable(id);
    }
    drawableIds.clear();

    if (features.empty()) {
        return;
    }

    const auto& layerImpl = impl(baseImpl);

    // Shared cube mesh + face-color texture across all instances.
    const auto sharedVertices = std::make_shared<model::CubeVertexVector>();
    const auto sharedIndices = std::make_shared<model::CubeIndexVector>();
    model::buildPlaceholderCube(*sharedVertices, *sharedIndices);
    const auto texture = model::createFaceColorTexture(context);

    for (const auto& feature : features) {
        const auto* point = feature.geometry.match(
            [](const mapbox::geometry::point<int16_t>& p) -> const mapbox::geometry::point<int16_t>* { return &p; },
            [](const auto&) -> const mapbox::geometry::point<int16_t>* { return nullptr; });
        if (!point) {
            continue;
        }

        // Tile coords in EXTENT units → mercator world fraction (zoom-free).
        const double fx = (tileId.x + static_cast<double>(point->x) / util::EXTENT) / tiles;
        const double fy = (tileId.y + static_cast<double>(point->y) / util::EXTENT) / tiles;
        const double lat = latitudeFromMercatorFraction(fy);

        const GeoJSONTileFeature tileFeature(feature);
        const float sizeMeters = evaluateFor(layerImpl.modelScale, tileFeature, 20.0f);
        const float rotationDeg = evaluateFor(layerImpl.modelRotation, tileFeature, 0.0f);
        const float opacity = layerImpl.modelOpacity.isUndefined()
                                  ? 1.0f
                                  : (layerImpl.modelOpacity.isConstant() ? layerImpl.modelOpacity.asConstant() : 1.0f);
        // model-id is parsed/evaluated to prove the seam; placeholder cubes ignore it.
        (void)evaluateFor(layerImpl.modelId, tileFeature, std::string{});

        CustomDrawableLayerHost::Interface::GeometryOptions options;
        options.texture = texture;
        options.color.a = opacity;

        interface.setGeometryOptions(options);
        interface.setGeometryTweakerCallback(
            [fx, fy, lat, sizeMeters, rotationDeg](gfx::Drawable&,
                                                   const PaintParameters& params,
                                                   CustomDrawableLayerHost::Interface::GeometryOptions& current) {
                const double worldSize = Projection::worldSize(params.state.getScale());
                const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(lat, params.state.getZoom());
                const double s = sizeMeters / metersPerPixel;

                mat4 m = matrix::identity4();
                matrix::translate(m, m, fx * worldSize, fy * worldSize, 0.0);
                matrix::rotate_z(m, m, util::deg2rad(rotationDeg));
                // x/y in world pixels, z in METERS (projection convention)
                matrix::scale(m, m, s, s, sizeMeters);
                matrix::multiply(current.matrix, params.transformParams.nearClippedProjMatrix, m);
            });

        drawableIds.push_back(interface.addGeometry(sharedVertices, sharedIndices, /*is3D=*/true));
    }

    interface.finish();
}

} // namespace mbgl
