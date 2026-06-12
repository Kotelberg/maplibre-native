#pragma once

#include <mbgl/renderer/model/glb_mesh_loader.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/identity.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#if MLN_WITH_FILAMENT_MODELS
#include <mbgl/renderer/model/filament_model_renderer.hpp>
#endif

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
    std::uint64_t lastPlacementKey = 0;
    std::uint64_t lastCameraKey = 0;

    std::vector<util::SimpleIdentity> drawableIds;

    // Static-mesh path: GLBs baked into map geometry (rock-solid under camera
    // motion, true per-pixel depth, unlit). Cached per model id.
    std::map<std::string, model::BakedModel> meshCache;

#if MLN_WITH_FILAMENT_MODELS
    std::unique_ptr<model::FilamentModelRenderer> filamentRenderer;
#endif
};

} // namespace mbgl
