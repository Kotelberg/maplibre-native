#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>

namespace mbgl {

// Minimal placeholder render layer for the experimental `model` type.
//
// This exists so the ModelLayerFactory (and therefore JSON style parsing and the
// layer's registration in the LayerManager) links and works. It performs no
// rendering yet: the glTF loading, placement, and custom-drawable submission are
// added in a follow-up commit that replaces this file wholesale.
class RenderModelLayer final : public RenderLayer {
public:
    explicit RenderModelLayer(Immutable<style::ModelLayer::Impl>);
    ~RenderModelLayer() override;

private:
    void transition(const TransitionParameters&) override;
    void evaluate(const PropertyEvaluationParameters&) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;
};

} // namespace mbgl
