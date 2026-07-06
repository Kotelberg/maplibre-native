#pragma once

#include <mbgl/layermanager/layer_factory.hpp>

namespace mbgl {

/// Factory for the experimental `model` layer type.
class ModelLayerFactory : public LayerFactory {
protected:
    const style::LayerTypeInfo* getTypeInfo() const noexcept final;
    std::unique_ptr<style::Layer> createLayer(const std::string& id,
                                              const style::conversion::Convertible& value) noexcept final;
    std::unique_ptr<RenderLayer> createRenderLayer(Immutable<style::Layer::Impl>) noexcept final;
    std::unique_ptr<Bucket> createBucket(const BucketParameters&,
                                         const std::vector<Immutable<style::LayerProperties>>&) noexcept final;
};

} // namespace mbgl
