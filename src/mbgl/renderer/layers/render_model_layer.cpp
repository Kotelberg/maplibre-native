#include <mbgl/renderer/layers/render_model_layer.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/model/placeholder_mesh.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/custom_geometry_ubo.hpp>
#include <mbgl/shaders/model_bloom_ubo.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/style/sources/geojson_source_impl.hpp>
#include <mbgl/tile/geojson_tile_data.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/tile_cover.hpp>

#include <chrono>
#include <cmath>
#include <optional>
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

// Soft radial contact shadow rendered under every model instance: grounds
// the object without a shadow-mapping pass.
std::shared_ptr<PremultipliedImage> makeContactShadowImage() {
    constexpr uint32_t kSize = 64;
    auto image = std::make_shared<PremultipliedImage>(Size{kSize, kSize});
    std::memset(image->data.get(), 0, image->bytes());
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) / kSize * 2.0 - 1.0;
            const double dy = (static_cast<double>(y) + 0.5) / kSize * 2.0 - 1.0;
            const double r = std::sqrt(dx * dx + dy * dy);
            const double falloff = std::max(0.0, 1.0 - r);
            const auto alpha = static_cast<uint8_t>(std::lround(falloff * falloff * 0.38 * 255.0));
            // Premultiplied black: rgb stay 0.
            image->data[(y * kSize + x) * 4 + 3] = alpha;
        }
    }
    return image;
}

// Models grow from the ground across z15→16 — the same linear ramp the style
// applies to fill-extrusion-height — so they rise in sync with the buildings
// around them (a full-size model fading in over half-grown buildings read as
// unsynced).
inline float zoomGrow(double zoom) {
    return static_cast<float>(std::clamp(zoom - 15.0, 0.0, 1.0));
}

// Short alpha ramp at the start of the growth window: at grow≈0 the mesh is
// collapsed onto the ground plane (every face coplanar), which z-fights.
inline float zoomFade(double zoom) {
    return static_cast<float>(std::clamp((zoom - 15.0) / 0.12, 0.0, 1.0));
}

// ── Bloom tuning ────────────────────────────────────────────────────
constexpr float kBloomIntensity = 0.85f;     // peak halo opacity
constexpr float kBloomPulseAmp = 0.18f;       // gentle breath around the peak
constexpr float kBloomPulsePeriod = 2.2f;     // seconds
constexpr float kBloomRadiusTexels = 10.0f;   // blur radius (in mask texels)
constexpr float kBloomColor[3] = {0.992f, 0.725f, 0.071f}; // #FDB912

struct BloomQuadVertex {
    std::array<float, 2> pos;
};

// Maps the selected model's anchor-relative meters → clip, exactly like the
// model's own per-frame tweaker, and paints it solid white into the offscreen
// mask via the custom-geometry shader.
class SilhouetteTweaker : public gfx::DrawableTweaker {
public:
    SilhouetteTweaker(double refFx_, double refFy_, double lat0_)
        : refFx(refFx_),
          refFy(refFy_),
          lat0(lat0_) {}
    void init(gfx::Drawable&) override {}
    void execute(gfx::Drawable& drawable, PaintParameters& params) override {
        const double worldSize = Projection::worldSize(params.state.getScale());
        const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(lat0, params.state.getZoom());
        const double pxPerMeter = 1.0 / metersPerPixel;
        mat4 m = matrix::identity4();
        matrix::translate(m, m, refFx * worldSize, refFy * worldSize, 0.0);
        matrix::scale(m, m, pxPerMeter, pxPerMeter, zoomGrow(params.state.getZoom()));
        mat4 mat;
        matrix::multiply(mat, params.transformParams.nearClippedProjMatrix, m);
        shaders::CustomGeometryDrawableUBO ubo{util::cast<float>(mat), Color::white()};
        drawable.mutableUniformBuffers().createOrUpdate(
            shaders::idCustomGeometryDrawableUBO, &ubo, params.context);
    }

private:
    double refFx, refFy, lat0;
};

// Drives the bloom composite UBO: glow colour + breathing intensity + blur
// step. The mask size is fixed for the drawable's lifetime.
class BloomCompositeTweaker : public gfx::DrawableTweaker {
public:
    explicit BloomCompositeTweaker(Size maskSize_)
        : maskSize(maskSize_) {}
    void init(gfx::Drawable&) override {}
    void execute(gfx::Drawable& drawable, PaintParameters& params) override {
        const auto now = std::chrono::steady_clock::now();
        static const auto t0 = now;
        const double t = std::chrono::duration<double>(now - t0).count();
        const float pulse = kBloomIntensity +
                            kBloomPulseAmp * static_cast<float>(
                                                 std::sin(t * (2.0 * M_PI / kBloomPulsePeriod)));
        shaders::ModelBloomDrawableUBO ubo{
            {kBloomColor[0], kBloomColor[1], kBloomColor[2], pulse},
            {1.0f / static_cast<float>(std::max(1u, maskSize.width)),
             1.0f / static_cast<float>(std::max(1u, maskSize.height))},
            kBloomRadiusTexels,
            0.0f};
        drawable.mutableUniformBuffers().createOrUpdate(
            shaders::idModelBloomDrawableUBO, &ubo, params.context);
    }

private:
    Size maskSize;
};

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

    // Per-frame early-out: with an unchanged cover, source, and style, the
    // placements cannot have changed — skip the feature walk entirely (it is
    // far too expensive to run per frame at thousands of features).
    std::uint64_t coverSig = 1469598103934665603ull;
    for (const auto& coverTile : cover) {
        const auto& c = coverTile.canonical;
        coverSig = (coverSig ^ ((std::uint64_t(c.z) << 58) ^ (std::uint64_t(c.x) << 29) ^ std::uint64_t(c.y))) *
                   1099511628211ull;
    }
    if (coverSig == lastCoverSig && lastData == data.get() && lastImpl == baseImpl.get()) {
        return;
    }
    lastCoverSig = coverSig;

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

    const float layerOpacity = layerImpl.modelOpacity.isUndefined()
                                   ? 1.0f
                                   : (layerImpl.modelOpacity.isConstant() ? layerImpl.modelOpacity.asConstant()
                                                                          : 1.0f);

    // Group instances by model so each (model, part) bakes into a handful of
    // merged drawables instead of one per feature — at thousands of features
    // (procedural trees) per-feature drawables collapse the frame rate.
    struct Instance {
        double fx;
        double fy;
        float size;
        float rotationDeg;
        float footprint;
    };
    std::map<std::string, std::vector<Instance>> groups;
    std::vector<Instance> cubes;
    // The selected model also renders normally; this captures it for the bloom.
    std::optional<std::pair<std::string, Instance>> selected;

    for (const auto& placed : features) {
        const GeoJSONTileFeature tileFeature(placed.feature);
        Instance instance{placed.fx,
                          placed.fy,
                          evaluateFor(layerImpl.modelScale, tileFeature, 20.0f),
                          evaluateFor(layerImpl.modelRotation, tileFeature, 0.0f),
                          evaluateFor(layerImpl.modelFootprint, tileFeature, 1.0f)};
        const std::string modelId = evaluateFor(layerImpl.modelId, tileFeature, std::string{});
        if (!modelId.empty() && layerImpl.modelAssets.count(modelId)) {
            groups[modelId].push_back(instance);
            if (!selected) {
                if (const auto value = tileFeature.getValue("selected")) {
                    if ((value->is<bool>() && value->get<bool>()) ||
                        (value->is<double>() && value->get<double>() != 0.0)) {
                        selected = std::make_pair(modelId, instance);
                    }
                }
            }
        } else {
            cubes.push_back(instance);
        }
    }

    using Vertex = CustomDrawableLayerHost::Interface::GeometryVertex;

    for (auto& [modelId, instances] : groups) {
        auto cacheIt = meshCache.find(modelId);
        if (cacheIt == meshCache.end()) {
            cacheIt = meshCache.emplace(modelId, model::loadGlbMesh(layerImpl.modelAssets.at(modelId))).first;
        }
        const auto& baked = cacheIt->second;
        if (!baked.valid) {
            cubes.insert(cubes.end(), instances.begin(), instances.end());
            continue;
        }

        // All instances of a group bake relative to the first instance's
        // anchor, in ground meters; the per-frame tweaker maps meters→pixels
        // at the anchor latitude. City-scale spans keep the mercator scale
        // error negligible.
        const Instance& ref = instances.front();
        const double refFx = ref.fx;
        const double refFy = ref.fy;
        const double lat0 = latitudeFromMercatorFraction(refFy);
        const double metersPerFraction = 40075016.686 * std::cos(util::deg2rad(lat0));

        // Contact shadows: one merged drawable of soft radial quads under the
        // group's instances, grounding the models.
        {
            if (!shadowTexture) {
                auto tex = context.createTexture2D();
                tex->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                              .wrapU = gfx::TextureWrapType::Clamp,
                                              .wrapV = gfx::TextureWrapType::Clamp});
                tex->setImage(makeContactShadowImage());
                shadowTexture = std::move(tex);
            }
            auto shadowVertices = std::make_shared<gfx::VertexVector<Vertex>>();
            auto shadowIndices = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
            for (const auto& inst : instances) {
                if (shadowVertices->elements() + 4 > 60000) break;
                const double cx = (inst.fx - refFx) * metersPerFraction;
                const double cy = (inst.fy - refFy) * metersPerFraction;
                const double half = inst.size * inst.footprint * 0.78;
                // A few cm above ground, scaled with model size, to dodge
                // ground-plane z-fighting.
                const double zLift = std::max(0.05, inst.size * 0.004);
                const auto base = static_cast<uint16_t>(shadowVertices->elements());
                shadowVertices->emplace_back(Vertex{{static_cast<float>(cx - half),
                                                     static_cast<float>(cy - half),
                                                     static_cast<float>(zLift)},
                                                    {0.f, 0.f}});
                shadowVertices->emplace_back(Vertex{{static_cast<float>(cx + half),
                                                     static_cast<float>(cy - half),
                                                     static_cast<float>(zLift)},
                                                    {1.f, 0.f}});
                shadowVertices->emplace_back(Vertex{{static_cast<float>(cx + half),
                                                     static_cast<float>(cy + half),
                                                     static_cast<float>(zLift)},
                                                    {1.f, 1.f}});
                shadowVertices->emplace_back(Vertex{{static_cast<float>(cx - half),
                                                     static_cast<float>(cy + half),
                                                     static_cast<float>(zLift)},
                                                    {0.f, 1.f}});
                // Double-sided: the world's south-positive y flips winding.
                shadowIndices->emplace_back(base, base + 1, base + 2);
                shadowIndices->emplace_back(base, base + 2, base + 3);
                shadowIndices->emplace_back(base, base + 2, base + 1);
                shadowIndices->emplace_back(base, base + 3, base + 2);
            }
            CustomDrawableLayerHost::Interface::GeometryOptions shadowOptions;
            shadowOptions.texture = shadowTexture;
            interface.setGeometryOptions(shadowOptions);
            interface.setGeometryTweakerCallback(
                [refFx, refFy, lat0](gfx::Drawable&,
                                     const PaintParameters& params,
                                     CustomDrawableLayerHost::Interface::GeometryOptions& current) {
                    const double worldSize = Projection::worldSize(params.state.getScale());
                    const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(
                        lat0, params.state.getZoom());
                    const double pxPerMeter = 1.0 / metersPerPixel;
                    mat4 m = matrix::identity4();
                    matrix::translate(m, m, refFx * worldSize, refFy * worldSize, 0.0);
                    matrix::scale(m, m, pxPerMeter, pxPerMeter, 1.0);
                    matrix::multiply(current.matrix, params.transformParams.nearClippedProjMatrix, m);
                    // Shadow deepens on the same ramp the model grows on.
                    const float fade = zoomGrow(params.state.getZoom());
                    current.color = {fade, fade, fade, fade};
                });
            drawableIds.push_back(interface.addGeometry(shadowVertices, shadowIndices, /*is3D=*/true));
        }

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
                partOptions.color.a = layerOpacity;
            } else {
                partOptions.color = part.color;
                partOptions.color.a *= layerOpacity;
            }

            const std::size_t partVertexCount = part.vertices->elements();
            constexpr std::size_t kMaxChunkVertices = 60000;

            std::shared_ptr<gfx::VertexVector<Vertex>> chunkVertices;
            std::shared_ptr<gfx::IndexVector<gfx::Triangles>> chunkIndices;

            const Color baseColor = partOptions.color;
            const auto flushChunk = [&] {
                if (!chunkVertices || chunkVertices->empty()) return;
                interface.setGeometryOptions(partOptions);
                interface.setGeometryTweakerCallback(
                    [refFx, refFy, lat0, baseColor](
                        gfx::Drawable&,
                        const PaintParameters& params,
                        CustomDrawableLayerHost::Interface::GeometryOptions& current) {
                        const double worldSize = Projection::worldSize(params.state.getScale());
                        const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(
                            lat0, params.state.getZoom());
                        const double pxPerMeter = 1.0 / metersPerPixel;

                        mat4 m = matrix::identity4();
                        matrix::translate(m, m, refFx * worldSize, refFy * worldSize, 0.0);
                        // x/y baked in ground meters → pixels; z stays meters
                        // (projection convention), scaled by the growth ramp so
                        // the model rises with the fill-extrusion buildings.
                        const double zoom = params.state.getZoom();
                        matrix::scale(m, m, pxPerMeter, pxPerMeter, zoomGrow(zoom));
                        matrix::multiply(current.matrix, params.transformParams.nearClippedProjMatrix, m);

                        // Premultiplied: scale all components.
                        const float fade = zoomFade(zoom);
                        current.color = {
                            baseColor.r * fade, baseColor.g * fade, baseColor.b * fade, baseColor.a * fade};
                    });
                drawableIds.push_back(interface.addGeometry(chunkVertices, chunkIndices, /*is3D=*/true));
                chunkVertices.reset();
                chunkIndices.reset();
            };

            for (const auto& inst : instances) {
                if (chunkVertices && chunkVertices->elements() + partVertexCount > kMaxChunkVertices) {
                    flushChunk();
                }
                if (!chunkVertices) {
                    chunkVertices = std::make_shared<gfx::VertexVector<Vertex>>();
                    chunkIndices = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
                }

                // Per-instance transform baked into vertices (ground meters
                // relative to the group anchor).
                mat4 f = matrix::identity4();
                matrix::translate(f,
                                  f,
                                  (inst.fx - refFx) * metersPerFraction,
                                  (inst.fy - refFy) * metersPerFraction,
                                  0.0);
                matrix::rotate_z(f, f, util::deg2rad(inst.rotationDeg));
                matrix::scale(
                    f, f, inst.size * inst.footprint, inst.size * inst.footprint, inst.size);

                const auto base = static_cast<uint16_t>(chunkVertices->elements());
                for (std::size_t vi = 0; vi < partVertexCount; ++vi) {
                    const Vertex& v = part.vertices->at(vi);
                    const vec4 p{v.position[0], v.position[1], v.position[2], 1.0};
                    vec4 out;
                    matrix::transformMat4(out, p, f);
                    chunkVertices->emplace_back(Vertex{
                        {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])},
                        v.texcoords});
                }
                const auto& idx = part.indices->vector();
                for (std::size_t ii = 0; ii + 2 < idx.size(); ii += 3) {
                    chunkIndices->emplace_back(static_cast<uint16_t>(base + idx[ii]),
                                               static_cast<uint16_t>(base + idx[ii + 1]),
                                               static_cast<uint16_t>(base + idx[ii + 2]));
                }
            }
            flushChunk();
        }
    }

    // Unresolved features keep the per-feature placeholder cube (rare).
    for (const auto& inst : cubes) {
        const double fx = inst.fx;
        const double fy = inst.fy;
        const double lat = latitudeFromMercatorFraction(fy);
        const float sizeMeters = inst.size;
        const float rotationDeg = inst.rotationDeg;

        CustomDrawableLayerHost::Interface::GeometryOptions options;
        options.texture = texture;
        options.color.a = layerOpacity;

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

    // ── Model-selection bloom ───────────────────────────────────────
    // The composite quad lives in this layer's main group (which the Interface
    // otherwise manages for the models); clear the previous one before rebuild.
    if (auto* mainGroup = static_cast<TileLayerGroup*>(layerGroup.get())) {
        mainGroup->removeDrawablesIf(
            [](gfx::Drawable& d) { return d.getName() == "modelBloomComposite"; });
    }

    if (selected && meshCache.count(selected->first) && meshCache.at(selected->first).valid) {
        const auto& baked = meshCache.at(selected->first);
        const Instance& inst = selected->second;
        const Size viewport = state.getSize();
        const Size maskSize{std::max(1u, viewport.width / 4u), std::max(1u, viewport.height / 4u)};

        if (!bloomShader) {
            bloomShader = context.getGenericShader(shaders, "ModelBloomShader");
        }
        if (!silhouetteShader) {
            silhouetteShader = context.getGenericShader(shaders, "CustomGeometryShader");
        }

        if (bloomShader && silhouetteShader) {
            if (!bloomWhiteTexture) {
                auto img = std::make_shared<PremultipliedImage>(Size{2, 2});
                img->fill(255);
                bloomWhiteTexture = context.createTexture2D();
                bloomWhiteTexture->setImage(std::move(img));
            }

            // (Re)create the offscreen mask on first use or viewport resize.
            if (!bloomTarget || bloomTargetSize.width != maskSize.width ||
                bloomTargetSize.height != maskSize.height) {
                if (bloomTarget && bloomTargetActive) {
                    changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(bloomTarget));
                }
                bloomTarget = context.createRenderTarget(maskSize, gfx::TextureChannelDataType::UnsignedByte);
                bloomTargetSize = maskSize;
                bloomTargetActive = false;
                if (bloomTarget) {
                    bloomTarget->getTexture()->setSamplerConfiguration(
                        {.filter = gfx::TextureFilterType::Linear,
                         .wrapU = gfx::TextureWrapType::Clamp,
                         .wrapV = gfx::TextureWrapType::Clamp});
                    bloomTarget->addLayerGroup(context.createTileLayerGroup(0, /*cap*/ 8, getID()),
                                               /*replace*/ true);
                    changes.emplace_back(std::make_unique<AddRenderTargetRequest>(bloomTarget));
                    bloomTargetActive = true;
                }
            }

            if (bloomTarget) {
                auto* maskGroup = static_cast<TileLayerGroup*>(bloomTarget->getLayerGroup(0).get());
                maskGroup->clearDrawables();

                const double lat0 = latitudeFromMercatorFraction(inst.fy);
                // Instance transform: rotate + scale only; the instance is its
                // own anchor, so the tweaker supplies the per-frame translation.
                mat4 f = matrix::identity4();
                matrix::rotate_z(f, f, util::deg2rad(inst.rotationDeg));
                matrix::scale(f, f, inst.size * inst.footprint, inst.size * inst.footprint, inst.size);

                const auto silTweaker = std::make_shared<SilhouetteTweaker>(inst.fx, inst.fy, lat0);

                for (const auto& part : baked.parts) {
                    const std::size_t partVertexCount = part.vertices->elements();
                    if (partVertexCount == 0) continue;
                    auto verts = std::make_shared<gfx::VertexVector<Vertex>>();
                    for (std::size_t vi = 0; vi < partVertexCount; ++vi) {
                        const Vertex& v = part.vertices->at(vi);
                        const vec4 p{v.position[0], v.position[1], v.position[2], 1.0};
                        vec4 out;
                        matrix::transformMat4(out, p, f);
                        verts->emplace_back(Vertex{{static_cast<float>(out[0]), static_cast<float>(out[1]),
                                                    static_cast<float>(out[2])},
                                                   v.texcoords});
                    }
                    auto attrs = context.createVertexAttributeArray();
                    if (const auto& a = attrs->set(shaders::idCustomGeometryPosVertexAttribute)) {
                        a->setSharedRawData(verts, offsetof(Vertex, position), 0, sizeof(Vertex),
                                            gfx::AttributeDataType::Float3);
                    }
                    if (const auto& a = attrs->set(shaders::idCustomGeometryTexVertexAttribute)) {
                        a->setSharedRawData(verts, offsetof(Vertex, texcoords), 0, sizeof(Vertex),
                                            gfx::AttributeDataType::Float2);
                    }
                    SegmentVector segs;
                    segs.emplace_back(0, 0, partVertexCount, part.indices->elements());

                    auto builder = context.createDrawableBuilder("modelBloomSilhouette");
                    builder->setShader(silhouetteShader);
                    builder->setEnableDepth(false);
                    builder->setColorMode(gfx::ColorMode::unblended());
                    builder->setCullFaceMode(gfx::CullFaceMode::disabled());
                    builder->setRenderPass(RenderPass::Translucent);
                    builder->setVertexAttributes(std::move(attrs));
                    builder->setRawVertices({}, partVertexCount, gfx::AttributeDataType::Float3);
                    builder->setSegments(gfx::Triangles(), part.indices, segs.data(), segs.size());
                    builder->setTexture(bloomWhiteTexture, shaders::idCustomGeometryTexture);
                    builder->flush(context);
                    for (auto& d : builder->clearDrawables()) {
                        d->setTileID({0, 0, 0});
                        d->addTweaker(silTweaker);
                        maskGroup->addDrawable(RenderPass::Translucent, {0, 0, 0}, std::move(d));
                    }
                }

                // Composite quad in the main group, drawn after the models.
                auto quad = std::make_shared<gfx::VertexVector<BloomQuadVertex>>();
                quad->emplace_back(BloomQuadVertex{{0.f, 0.f}});
                quad->emplace_back(BloomQuadVertex{{1.f, 0.f}});
                quad->emplace_back(BloomQuadVertex{{0.f, 1.f}});
                quad->emplace_back(BloomQuadVertex{{1.f, 1.f}});
                std::vector<uint16_t> quadIdx{0, 1, 2, 1, 2, 3};
                SegmentVector quadSegs;
                quadSegs.emplace_back(0, 0, 4, 6);

                auto qattrs = context.createVertexAttributeArray();
                if (const auto& a = qattrs->set(shaders::idModelBloomPosVertexAttribute)) {
                    a->setSharedRawData(quad, offsetof(BloomQuadVertex, pos), 0, sizeof(BloomQuadVertex),
                                        gfx::AttributeDataType::Float2);
                }
                auto cbuilder = context.createDrawableBuilder("modelBloomComposite");
                cbuilder->setShader(bloomShader);
                cbuilder->setEnableDepth(false);
                cbuilder->setColorMode(gfx::ColorMode::additive());
                cbuilder->setCullFaceMode(gfx::CullFaceMode::disabled());
                cbuilder->setRenderPass(RenderPass::Translucent);
                cbuilder->setVertexAttributes(std::move(qattrs));
                cbuilder->setRawVertices({}, 4, gfx::AttributeDataType::Float2);
                cbuilder->setSegments(gfx::Triangles(), std::move(quadIdx), quadSegs.data(), quadSegs.size());
                cbuilder->setTexture(bloomTarget->getTexture(), shaders::idModelBloomImageTexture);
                cbuilder->flush(context);
                const auto compTweaker = std::make_shared<BloomCompositeTweaker>(maskSize);
                if (auto* mainGroup = static_cast<TileLayerGroup*>(layerGroup.get())) {
                    for (auto& d : cbuilder->clearDrawables()) {
                        d->setName("modelBloomComposite");
                        d->setTileID({0, 0, 0});
                        d->addTweaker(compTweaker);
                        mainGroup->addDrawable(RenderPass::Translucent, {0, 0, 0}, std::move(d));
                    }
                }
            }
        }
    } else {
        teardownBloom(changes);
    }
}

void RenderModelLayer::teardownBloom(UniqueChangeRequestVec& changes) {
    if (bloomTarget && bloomTargetActive) {
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(bloomTarget));
    }
    bloomTarget.reset();
    bloomTargetActive = false;
    bloomTargetSize = Size{0, 0};
    if (auto* mainGroup = static_cast<TileLayerGroup*>(layerGroup.get())) {
        mainGroup->removeDrawablesIf(
            [](gfx::Drawable& d) { return d.getName() == "modelBloomComposite"; });
    }
}

} // namespace mbgl
