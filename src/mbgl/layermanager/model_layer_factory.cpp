#include <mbgl/layermanager/model_layer_factory.hpp>

#include <mbgl/renderer/layers/render_model_layer.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {

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

} // namespace mbgl
