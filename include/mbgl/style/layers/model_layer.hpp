#pragma once

#include <mbgl/style/layer.hpp>
#include <mbgl/style/property_value.hpp>

#include <string>

namespace mbgl {
namespace style {

/// Experimental `model` style layer (fork extension, not part of the MapLibre
/// style spec): places a 3D model per Point feature of a GeoJSON source.
/// M3a renders placeholder cubes; Filament-backed glTF rendering lands in M3b.
///
/// Properties (data-driven where noted):
///   layout: model-id (string, data-driven)
///   paint:  model-scale (meters, data-driven), model-rotation (yaw degrees,
///           data-driven), model-opacity (constant)
class ModelLayer final : public Layer {
public:
    ModelLayer(const std::string& layerID, const std::string& sourceID);
    ~ModelLayer() override;

    // Layout properties
    static PropertyValue<std::string> getDefaultModelId();
    const PropertyValue<std::string>& getModelId() const;
    void setModelId(const PropertyValue<std::string>&);

    // Paint properties
    static PropertyValue<float> getDefaultModelScale();
    const PropertyValue<float>& getModelScale() const;
    void setModelScale(const PropertyValue<float>&);

    static PropertyValue<float> getDefaultModelRotation();
    const PropertyValue<float>& getModelRotation() const;
    void setModelRotation(const PropertyValue<float>&);

    static PropertyValue<float> getDefaultModelOpacity();
    const PropertyValue<float>& getModelOpacity() const;
    void setModelOpacity(const PropertyValue<float>&);

    // Private implementation
    class Impl;
    const Impl& impl() const;

    Mutable<Impl> mutableImpl() const;
    ModelLayer(Immutable<Impl>);
    std::unique_ptr<Layer> cloneRef(const std::string& id) const final;

protected:
    std::optional<conversion::Error> setPropertyInternal(const std::string& name,
                                                         const conversion::Convertible& value) final;
    StyleProperty getProperty(const std::string& name) const final;

    Mutable<Layer::Impl> mutableBaseImpl() const final;
};

} // namespace style
} // namespace mbgl
