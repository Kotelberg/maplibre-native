#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/identity.hpp>

#include <vector>

namespace mbgl {

/// M3a render layer for `type: "model"`: reads Point features synchronously
/// from the layer's GeoJSON source (z0 tile = whole dataset) and renders one
/// placeholder cube per feature through the custom-drawable geometry path.
/// Filament-backed glTF visuals replace the cubes in M3b.
class RenderModelLayer final : public RenderLayer {
public:
    explicit RenderModelLayer(Immutable<style::ModelLayer::Impl>);
    ~RenderModelLayer() override;

    void update(gfx::ShaderRegistry&,
                gfx::Context&,
                const TransformState&,
                const std::shared_ptr<UpdateParameters>&,
                const RenderTree&,
                UniqueChangeRequestVec&) override;

private:
    void transition(const TransitionParameters&) override {}
    void evaluate(const PropertyEvaluationParameters&) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;
    void prepare(const LayerPrepareParameters&) override;

    // change detection for drawable rebuilds
    const void* lastImpl = nullptr;
    const void* lastData = nullptr;
    std::size_t lastFeatureCount = 0;
    CanonicalTileID lastTile{0, 0, 0};

    std::vector<util::SimpleIdentity> drawableIds;
};

} // namespace mbgl
