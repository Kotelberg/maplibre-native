#include <mbgl/layermanager/model_layer_factory.hpp>

#include <mbgl/renderer/bucket.hpp>
#include <mbgl/renderer/layers/render_model_layer.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {

namespace {

// The model render layer reads the GeoJSON source features directly rather than
// consuming per-tile buckets, so the tile worker has no model geometry to build.
// hasData()==false makes the tile worker drop this bucket.
class NoopModelBucket final : public Bucket {
public:
    void addFeature(const GeometryTileFeature&,
                    const GeometryCollection&,
                    const ImagePositions&,
                    const PatternLayerMap&,
                    std::size_t,
                    const CanonicalTileID&) override {}
    void upload(gfx::UploadPass&) override {}
    bool hasData() const override { return false; }
};

} // namespace

const style::LayerTypeInfo* ModelLayerFactory::getTypeInfo() const noexcept {
    return style::ModelLayer::Impl::staticTypeInfo();
}

std::unique_ptr<style::Layer> ModelLayerFactory::createLayer(const std::string& id,
                                                             const style::conversion::Convertible& value) noexcept {
    const auto source = getSource(value);
    if (!source) {
        return nullptr;
    }
    return std::unique_ptr<style::Layer>(new style::ModelLayer(id, *source));
}

std::unique_ptr<RenderLayer> ModelLayerFactory::createRenderLayer(Immutable<style::Layer::Impl> impl) noexcept {
    assert(impl->getTypeInfo() == getTypeInfo());
    return std::make_unique<RenderModelLayer>(staticImmutableCast<style::ModelLayer::Impl>(impl));
}

std::unique_ptr<Bucket> ModelLayerFactory::createBucket(const BucketParameters&,
                                                        const std::vector<Immutable<style::LayerProperties>>&) noexcept {
    return std::make_unique<NoopModelBucket>();
}

} // namespace mbgl
