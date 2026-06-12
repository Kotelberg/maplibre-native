#pragma once

#include "MLNStyleLayer_Private.h"

#include <mbgl/layermanager/model_layer_factory.hpp>

namespace mbgl {

class ModelStyleLayerPeerFactory : public LayerPeerFactory, public mbgl::ModelLayerFactory {
    // LayerPeerFactory overrides.
    LayerFactory* getCoreLayerFactory() final { return this; }
    virtual MLNStyleLayer* createPeer(style::Layer*) final;
};

}  // namespace mbgl
