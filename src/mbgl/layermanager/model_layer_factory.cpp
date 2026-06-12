#include <mbgl/layermanager/model_layer_factory.hpp>

#include <mbgl/renderer/bucket.hpp>
#include <mbgl/renderer/layers/render_model_layer.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {

namespace {

// M3a: the render layer reads the GeoJSON source directly; tiles carry no
// model data yet. hasData()==false makes the tile worker drop the bucket.
// The real per-tile ModelBucket lands in M5 (tile-based lifecycle).
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
