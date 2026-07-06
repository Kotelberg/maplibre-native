#include <mbgl/renderer/layers/render_model_layer.hpp>

#include <mbgl/renderer/property_evaluation_parameters.hpp>
#include <mbgl/style/layers/model_layer_properties.hpp>

namespace mbgl {

using namespace style;

RenderModelLayer::RenderModelLayer(Immutable<style::ModelLayer::Impl> _impl)
    : RenderLayer(makeMutable<ModelLayerProperties>(std::move(_impl))) {}

RenderModelLayer::~RenderModelLayer() = default;

void RenderModelLayer::transition(const TransitionParameters&) {}

void RenderModelLayer::evaluate(const PropertyEvaluationParameters&) {
    // The placeholder layer keeps its unevaluated properties; nothing is drawn.
    evaluatedProperties = makeMutable<ModelLayerProperties>(staticImmutableCast<ModelLayer::Impl>(baseImpl));
    passes = RenderPass::None;
}

bool RenderModelLayer::hasTransition() const {
    return false;
}

bool RenderModelLayer::hasCrossfade() const {
    return false;
}

} // namespace mbgl
