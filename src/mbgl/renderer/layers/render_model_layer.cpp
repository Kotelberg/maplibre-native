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
#include <mbgl/util/tile_cover.hpp>

#include <cmath>
#include <unordered_set>

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

    // M5 tile lifecycle: read features from every tile in the viewport cover
    // at the current integer zoom (z0 int16 tile coordinates quantize to
    // ~5 km cells, far too coarse for placement). Tile buffers repeat
    // features near borders, so placements deduplicate on the quantized
    // world-fraction anchor.
    const auto z = static_cast<uint8_t>(std::clamp(static_cast<int>(state.getZoom()), 0, 18));
    const double tiles = static_cast<double>(1u << z);

    auto cover = util::tileCover(util::TileCoverParameters{.transformState = state}, z, Range<uint8_t>{z, z});
    // Backstop against degenerate covers (extreme pitch at low zoom).
    constexpr std::size_t kMaxCoverTiles = 64;
    if (cover.size() > kMaxCoverTiles) {
        cover.erase(cover.begin() + kMaxCoverTiles, cover.end());
    }

    struct PlacedFeature {
        mapbox::feature::feature<std::int16_t> feature;
        double fx;
        double fy;
    };
    std::vector<PlacedFeature> features;
    std::unordered_set<std::uint64_t> seenAnchors;
    std::uint64_t placementKey = 0;
    for (const auto& coverTile : cover) {
        const auto& canonical = coverTile.canonical;
        GeoJSONData::TileFeatures tileFeatures;
        data->getTile(canonical, [&](GeoJSONData::TileFeatures f) { tileFeatures = std::move(f); },
                      /*runSynchronously=*/true);
        for (auto& feature : tileFeatures) {
            const auto* point = feature.geometry.match(
                [](const mapbox::geometry::point<int16_t>& p) -> const mapbox::geometry::point<int16_t>* {
                    return &p;
                },
                [](const auto&) -> const mapbox::geometry::point<int16_t>* { return nullptr; });
            if (!point) {
                continue;
            }
            // Tile coords in EXTENT units → mercator world fraction (zoom-free).
            const double fx = (canonical.x + static_cast<double>(point->x) / util::EXTENT) / tiles;
            const double fy = (canonical.y + static_cast<double>(point->y) / util::EXTENT) / tiles;
            const auto qx = static_cast<std::uint64_t>(std::llround(fx * 4294967296.0)) & 0xFFFFFFFFull;
            const auto qy = static_cast<std::uint64_t>(std::llround(fy * 4294967296.0)) & 0xFFFFFFFFull;
            const std::uint64_t anchorKey = (qx << 32) | qy;
            if (!seenAnchors.insert(anchorKey).second) {
                continue;
            }
            placementKey ^= anchorKey * 0x9E3779B97F4A7C15ull;
            features.push_back(PlacedFeature{std::move(feature), fx, fy});
        }
    }

    const auto& layerImplRef = impl(baseImpl);

    // Rebuild drawables only when the style impl, source data, or placements
    // changed (baked meshes are camera-independent; the tweaker matrix tracks
    // the camera per frame).
    const bool changed = lastImpl != baseImpl.get() || lastData != data.get() ||
                         lastFeatureCount != features.size() || lastPlacementKey != placementKey;
    if (!changed) {
        return;
    }
    lastImpl = baseImpl.get();
    lastData = data.get();
    lastFeatureCount = features.size();
    lastPlacementKey = placementKey;

    CustomDrawableLayerHost::Interface interface(
        *this, layerGroup, shaders, context, state, updateParameters, renderTree, changes);

    for (const auto& id : drawableIds) {
        interface.removeDrawable(id);
    }
    drawableIds.clear();

    if (features.empty()) {
        return;
    }

    const auto& layerImpl = layerImplRef;

    // Shared cube mesh + face-color texture across all instances.
    const auto sharedVertices = std::make_shared<model::CubeVertexVector>();
    const auto sharedIndices = std::make_shared<model::CubeIndexVector>();
    model::buildPlaceholderCube(*sharedVertices, *sharedIndices);
    const auto texture = model::createFaceColorTexture(context);

    // GPU textures for baked-mesh parts, shared across features this rebuild
    std::map<const PremultipliedImage*, gfx::Texture2DPtr> partTextures;

    for (const auto& placed : features) {
        const auto& feature = placed.feature;
        const double fx = placed.fx;
        const double fy = placed.fy;
        const double lat = latitudeFromMercatorFraction(fy);

        const GeoJSONTileFeature tileFeature(feature);
        const float sizeMeters = evaluateFor(layerImpl.modelScale, tileFeature, 20.0f);
        const float rotationDeg = evaluateFor(layerImpl.modelRotation, tileFeature, 0.0f);
        const float opacity = layerImpl.modelOpacity.isUndefined()
                                  ? 1.0f
                                  : (layerImpl.modelOpacity.isConstant() ? layerImpl.modelOpacity.asConstant() : 1.0f);
        const float footprint = evaluateFor(layerImpl.modelFootprint, tileFeature, 1.0f);
        const std::string modelId = evaluateFor(layerImpl.modelId, tileFeature, std::string{});

        if (!modelId.empty() && layerImpl.modelAssets.count(modelId)) {
            // Static-mesh path (default): GLB baked into map geometry — same
            // tweaker-matrix mechanics as the cube, so placement is rigid
            // under all camera motion and depth is per-pixel correct.
            auto cacheIt = meshCache.find(modelId);
            if (cacheIt == meshCache.end()) {
                cacheIt = meshCache.emplace(modelId, model::loadGlbMesh(layerImpl.modelAssets.at(modelId))).first;
            }
            const auto& baked = cacheIt->second;
            if (baked.valid) {
                for (const auto& part : baked.parts) {
                    CustomDrawableLayerHost::Interface::GeometryOptions partOptions;
                    if (part.texture) {
                        if (!partTextures.count(part.texture.get())) {
                            auto tex = context.createTexture2D();
                            tex->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                          .wrapU = gfx::TextureWrapType::Repeat,
                                                          .wrapV = gfx::TextureWrapType::Repeat,
                                                          .mipmapped = true});
                            tex->setImage(part.texture);
                            partTextures[part.texture.get()] = std::move(tex);
                        }
                        partOptions.texture = partTextures[part.texture.get()];
                        partOptions.color.a = opacity;
                    } else {
                        partOptions.color = part.color;
                        partOptions.color.a *= opacity;
                    }

                    interface.setGeometryOptions(partOptions);
                    interface.setGeometryTweakerCallback(
                        [fx, fy, lat, sizeMeters, rotationDeg, footprint](
                            gfx::Drawable&,
                            const PaintParameters& params,
                            CustomDrawableLayerHost::Interface::GeometryOptions& current) {
                            const double worldSize = Projection::worldSize(params.state.getScale());
                            const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(
                                lat, params.state.getZoom());
                            const double s = sizeMeters / metersPerPixel * footprint;

                            mat4 m = matrix::identity4();
                            matrix::translate(m, m, fx * worldSize, fy * worldSize, 0.0);
                            matrix::rotate_z(m, m, util::deg2rad(rotationDeg));
                            matrix::scale(m, m, s, s, sizeMeters);
                            matrix::multiply(
                                current.matrix, params.transformParams.nearClippedProjMatrix, m);
                        });
                    drawableIds.push_back(interface.addGeometry(part.vertices, part.indices, /*is3D=*/true));
                }
                continue;
            }
        }

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
