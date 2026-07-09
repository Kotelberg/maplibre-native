#pragma once

#include <mbgl/style/layer_impl.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layers/model_layer_properties.hpp>

#include <map>
#include <string>

namespace mbgl {
namespace style {

class ModelLayer::Impl : public Layer::Impl {
public:
    using Layer::Impl::Impl;

    bool hasLayoutDifference(const Layer::Impl&) const override;
    void stringifyLayout(rapidjson::Writer<rapidjson::StringBuffer>&) const override;

    expression::Dependency getDependencies() const noexcept override {
        return layout.getDependencies() | paint.getDependencies();
    }

    ModelLayoutProperties::Unevaluated layout;
    ModelPaintProperties::Transitionable paint;

    /// Runtime asset registry (not part of the style spec): maps a `model-id`
    /// value to a local glTF/GLB file path. Populated through the SDK-facing
    /// ModelLayer::setModelAssets, mirroring the image-registry precedent
    /// (Style::addImage). The render layer resolves `model-id` against this map.
    std::map<std::string, std::string> modelAssets;

    DECLARE_LAYER_TYPE_INFO;
};

} // namespace style
} // namespace mbgl
