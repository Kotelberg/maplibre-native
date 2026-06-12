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

    const auto& layerImplRef = impl(baseImpl);

    // Camera key matters once Filament-rendered models exist (the offscreen
    // image bakes the camera); quantized to avoid re-render churn.
    std::uint64_t cameraKey = 0;
    if (!layerImplRef.modelAssets.empty()) {
        const auto q = [](double v) {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(v * 256.0)) & 0xFFFF;
        };
        cameraKey = (q(state.getZoom()) << 48) ^ (q(state.getBearing()) << 32) ^ (q(state.getPitch()) << 16) ^
                    q(state.getLatLng().latitude() * 64) ^ (q(state.getLatLng().longitude() * 64) << 8);
        cameraKey |= 1; // distinguish "camera tracked" from the initial 0
    }

    // Rebuild drawables only when the style impl, source data, tile, or
    // (for Filament content) camera changed.
    const bool changed = lastImpl != baseImpl.get() || lastData != data.get() ||
                         lastFeatureCount != features.size() || !(lastTile == tileId) ||
                         lastCameraKey != cameraKey;
    if (!changed) {
        return;
    }
    lastImpl = baseImpl.get();
    lastData = data.get();
    lastFeatureCount = features.size();
    lastTile = tileId;
    lastCameraKey = cameraKey;

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

#if MLN_WITH_FILAMENT_MODELS
    std::vector<model::ModelInstanceSpec> filamentSpecs;
#endif

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
#if MLN_WITH_FILAMENT_MODELS
            // Fallback: Filament billboard composite (PBR lighting, but the
            // image is camera-baked — wobbles under interaction).
            filamentSpecs.push_back(model::ModelInstanceSpec{
                modelId, fx, fy, lat, sizeMeters, rotationDeg, opacity});
            continue;
#endif
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

#if MLN_WITH_FILAMENT_MODELS
    if (!filamentSpecs.empty()) {
        if (!filamentRenderer) {
            filamentRenderer = std::make_unique<model::FilamentModelRenderer>();
        }
        filamentRenderer->setAssets(layerImpl.modelAssets);

        const auto viewport = state.getSize();
        const double worldSize = Projection::worldSize(state.getScale());
        mat4 proj;
        // Same construction as PaintParameters::nearClippedProjMatrix
        state.getProjMatrix(proj, static_cast<uint16_t>(0.1 * state.getCameraToCenterDistance()));
        const Point<double> anchorPx = Projection::project(state.getLatLng(), state.getScale());

        // Per-model billboard with depth (M4): each model renders into its
        // own cropped image; the textured quad sits at the anchor's clip-space
        // depth with is3D depth read/write, so extruded buildings occlude it.
        using Vertex = CustomDrawableLayerHost::Interface::GeometryVertex;
        auto quadVertices = std::make_shared<gfx::VertexVector<Vertex>>();
        auto quadIndices = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
        quadVertices->emplace_back(Vertex{.position = {-1, -1, 0}, .texcoords = {0, 1}});
        quadVertices->emplace_back(Vertex{.position = {1, -1, 0}, .texcoords = {1, 1}});
        quadVertices->emplace_back(Vertex{.position = {1, 1, 0}, .texcoords = {1, 0}});
        quadVertices->emplace_back(Vertex{.position = {-1, 1, 0}, .texcoords = {0, 0}});
        quadIndices->emplace_back(0, 1, 2);
        quadIndices->emplace_back(0, 2, 3);

        for (const auto& spec : filamentSpecs) {
            const double wx = spec.worldFractionX * worldSize;
            const double wy = spec.worldFractionY * worldSize;

            // Anchor in map clip space
            const double cw = proj[3] * wx + proj[7] * wy + proj[15];
            if (cw <= 0) continue;
            const double cx = (proj[0] * wx + proj[4] * wy + proj[12]) / cw;
            const double cy = (proj[1] * wx + proj[5] * wy + proj[13]) / cw;
            if (std::abs(cx) > 1.5 || std::abs(cy) > 1.5) continue;

            const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(spec.latitude, state.getZoom());
            const double halfPx = std::max(8.0, spec.sizeMeters / metersPerPixel * 1.6);
            model::FilamentModelRenderer::CropRect cropRect{
                cx, cy, 2.0 * halfPx / viewport.width, 2.0 * halfPx / viewport.height};

            const auto texSize = static_cast<uint32_t>(
                std::clamp(2.0 * halfPx, 16.0, static_cast<double>(std::max(viewport.width, viewport.height))));

            auto image = filamentRenderer->render({spec},
                                                  proj,
                                                  anchorPx.x,
                                                  anchorPx.y,
                                                  worldSize,
                                                  state.getZoom(),
                                                  texSize,
                                                  texSize,
                                                  &cropRect);
            if (!image) continue;

            auto overlayTexture = context.createTexture2D();
            overlayTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                     .wrapU = gfx::TextureWrapType::Clamp,
                                                     .wrapV = gfx::TextureWrapType::Clamp});
            overlayTexture->setImage(std::move(image));

            CustomDrawableLayerHost::Interface::GeometryOptions overlayOptions;
            overlayOptions.texture = std::move(overlayTexture);

            interface.setGeometryOptions(overlayOptions);
            interface.setGeometryTweakerCallback(
                [wx, wy, spec, halfPx](gfx::Drawable&,
                                       const PaintParameters& params,
                                       CustomDrawableLayerHost::Interface::GeometryOptions& current) {
                    // Recompute the clip rect per frame (cheap; camera changes
                    // trigger a full re-render anyway, this keeps it aligned).
                    const auto& p = params.transformParams.nearClippedProjMatrix;
                    const double w = p[3] * wx + p[7] * wy + p[15];
                    if (w <= 0) {
                        current.matrix = matrix::identity4();
                        matrix::scale(current.matrix, current.matrix, 0, 0, 0);
                        return;
                    }
                    const double ncx = (p[0] * wx + p[4] * wy + p[12]) / w;
                    const double ncy = (p[1] * wx + p[5] * wy + p[13]) / w;
                    const double nz = (p[2] * wx + p[6] * wy + p[14]) / w;
                    const auto size = params.state.getSize();
                    const double nhx = 2.0 * halfPx / size.width;
                    const double nhy = 2.0 * halfPx / size.height;

                    mat4 m = matrix::identity4();
                    m[0] = nhx;
                    m[5] = nhy;
                    m[10] = 0.0;
                    m[12] = ncx;
                    m[13] = ncy;
                    m[14] = nz;
                    current.matrix = m;
                });
            drawableIds.push_back(interface.addGeometry(quadVertices, quadIndices, /*is3D=*/true));
        }
    }
#endif

    interface.finish();
}

} // namespace mbgl
