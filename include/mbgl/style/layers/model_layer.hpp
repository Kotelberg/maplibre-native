// clang-format off

// This file is generated. Do not edit.

#pragma once

#include <mbgl/style/layer.hpp>
#include <mbgl/style/filter.hpp>
#include <mbgl/style/property_value.hpp>
#include <map>
#include <mbgl/util/color.hpp>

namespace mbgl {
namespace style {

class TransitionOptions;

class ModelLayer final : public Layer {
public:
    ModelLayer(const std::string& layerID, const std::string& sourceID);
    ~ModelLayer() override;

    // Layout properties

    static PropertyValue<std::string> getDefaultModelId();
    const PropertyValue<std::string>& getModelId() const;
    void setModelId(const PropertyValue<std::string>&);

    // Paint properties

    static PropertyValue<float> getDefaultModelFootprint();
    const PropertyValue<float>& getModelFootprint() const;
    void setModelFootprint(const PropertyValue<float>&);
    void setModelFootprintTransition(const TransitionOptions&);
    TransitionOptions getModelFootprintTransition() const;

    static PropertyValue<float> getDefaultModelOpacity();
    const PropertyValue<float>& getModelOpacity() const;
    void setModelOpacity(const PropertyValue<float>&);
    void setModelOpacityTransition(const TransitionOptions&);
    TransitionOptions getModelOpacityTransition() const;

    static PropertyValue<float> getDefaultModelRotation();
    const PropertyValue<float>& getModelRotation() const;
    void setModelRotation(const PropertyValue<float>&);
    void setModelRotationTransition(const TransitionOptions&);
    TransitionOptions getModelRotationTransition() const;

    static PropertyValue<float> getDefaultModelScale();
    const PropertyValue<float>& getModelScale() const;
    void setModelScale(const PropertyValue<float>&);
    void setModelScaleTransition(const TransitionOptions&);
    TransitionOptions getModelScaleTransition() const;

    // Model asset registry (runtime SDK API; not part of the style spec). Maps a
    // `model-id` value to a local glTF/GLB file path. Mirrors Style::addImage: the
    // host registers assets at runtime, while the style references them only by id.
    const std::map<std::string, std::string>& getModelAssets() const;
    void setModelAssets(std::map<std::string, std::string>);

    // Private implementation

    class Impl;
    const Impl& impl() const;

    Mutable<Impl> mutableImpl() const;
    ModelLayer(Immutable<Impl>);
    std::unique_ptr<Layer> cloneRef(const std::string& id) const final;

protected:
    // Dynamic properties
    std::optional<conversion::Error> setPropertyInternal(const std::string& name, const conversion::Convertible& value) final;

    StyleProperty getProperty(const std::string& name) const final;
    Value serialize() const final;

    Mutable<Layer::Impl> mutableBaseImpl() const final;
};

} // namespace style
} // namespace mbgl

// clang-format on
