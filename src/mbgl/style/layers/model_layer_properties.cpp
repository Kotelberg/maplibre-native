// clang-format off

// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

#include <mbgl/style/layers/model_layer_properties.hpp>

#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {
namespace style {

ModelLayerProperties::ModelLayerProperties(
    Immutable<ModelLayer::Impl> impl_)
    : LayerProperties(std::move(impl_)) {}

ModelLayerProperties::ModelLayerProperties(
    Immutable<ModelLayer::Impl> impl_,
    ModelPaintProperties::PossiblyEvaluated evaluated_)
  : LayerProperties(std::move(impl_)),
    evaluated(std::move(evaluated_)) {}

ModelLayerProperties::~ModelLayerProperties() = default;

unsigned long ModelLayerProperties::constantsMask() const {
    return evaluated.constantsMask();
}

const ModelLayer::Impl& ModelLayerProperties::layerImpl() const noexcept {
    return static_cast<const ModelLayer::Impl&>(*baseImpl);
}

expression::Dependency ModelLayerProperties::getDependencies() const noexcept {
    return layerImpl().paint.getDependencies() | layerImpl().layout.getDependencies();
}

} // namespace style
} // namespace mbgl

// clang-format on
