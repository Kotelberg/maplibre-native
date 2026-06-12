#pragma once

#include <mbgl/style/layer_impl.hpp>
#include <mbgl/style/layer_properties.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/property_value.hpp>

namespace mbgl {
namespace style {

class ModelLayer::Impl : public Layer::Impl {
public:
    using Layer::Impl::Impl;

    bool hasLayoutDifference(const Layer::Impl&) const override;
    void stringifyLayout(rapidjson::Writer<rapidjson::StringBuffer>&) const override;

    PropertyValue<std::string> modelId;
    PropertyValue<float> modelScale;
    PropertyValue<float> modelRotation;
    PropertyValue<float> modelOpacity;

    DECLARE_LAYER_TYPE_INFO;
};

class ModelLayerProperties final : public LayerProperties {
public:
    explicit ModelLayerProperties(Immutable<ModelLayer::Impl> impl)
        : LayerProperties(std::move(impl)) {}

    expression::Dependency getDependencies() const noexcept override { return expression::Dependency::None; }
};

} // namespace style
} // namespace mbgl
