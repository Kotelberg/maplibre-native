#pragma once

#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/util/geo.hpp>

namespace mbgl {
namespace style {

/// M1 debug host: renders a colored cube with edge `sizeMeters` anchored at
/// `location`, sitting on the ground plane. Validates placement math, depth
/// read/write vs fill-extrusion, and layer ordering ahead of the Filament
/// model layer. Not part of the public style spec; remove after M4.
class DebugCubeLayerHost : public CustomDrawableLayerHost {
public:
    DebugCubeLayerHost(LatLng location, double sizeMeters);

    void initialize() override;
    void update(Interface& interface) override;
    void deinitialize() override;

private:
    const LatLng location;
    const double sizeMeters;
};

} // namespace style
} // namespace mbgl
