// clang-format off

// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

#pragma once

#include <mbgl/style/types.hpp>
#include <mbgl/style/layer_properties.hpp>
#include <mbgl/style/layers/model_layer.hpp>
#include <mbgl/style/layout_property.hpp>
#include <mbgl/style/paint_property.hpp>
#include <mbgl/style/properties.hpp>
#include <mbgl/shaders/attributes.hpp>
#include <mbgl/shaders/uniforms.hpp>

namespace mbgl {
namespace style {

struct ModelId : DataDrivenLayoutProperty<std::string> {
    static constexpr const char *name() { return "model-id"; }
    static std::string defaultValue() { return {}; }
};

struct ModelFootprint : DataDrivenPaintProperty<float, attributes::model_footprint, uniforms::model_footprint> {
    static float defaultValue() { return 1.f; }
};

struct ModelOpacity : PaintProperty<float> {
    static float defaultValue() { return 1.f; }
};

struct ModelRotation : DataDrivenPaintProperty<float, attributes::model_rotation, uniforms::model_rotation> {
    static float defaultValue() { return 0.f; }
};

struct ModelScale : DataDrivenPaintProperty<float, attributes::model_scale, uniforms::model_scale> {
    static float defaultValue() { return 20.f; }
};

class ModelLayoutProperties : public Properties<
    ModelId
> {};

class ModelPaintProperties : public Properties<
    ModelFootprint,
    ModelOpacity,
    ModelRotation,
    ModelScale
> {};

class ModelLayerProperties final : public LayerProperties {
public:
    explicit ModelLayerProperties(Immutable<ModelLayer::Impl>);
    ModelLayerProperties(
        Immutable<ModelLayer::Impl>,
        ModelPaintProperties::PossiblyEvaluated);
    ~ModelLayerProperties() override;

    unsigned long constantsMask() const override;

    expression::Dependency getDependencies() const noexcept override;

    const ModelLayer::Impl& layerImpl() const noexcept;
    // Data members.
    ModelPaintProperties::PossiblyEvaluated evaluated;
};

} // namespace style
} // namespace mbgl

// clang-format on
