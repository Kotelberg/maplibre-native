#pragma once

#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/renderer/model/glb_mesh_loader.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layers/model_layer_impl.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/identity.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace mbgl {

/// Render layer for `type: "model"` (fork extension): reads Point features
/// from the viewport tile cover of the layer's GeoJSON source and renders
/// each feature's glTF model through the custom-drawable geometry path
/// (meshes baked into map space — rigid under camera motion, per-pixel
/// depth). Features without a resolvable model render a placeholder cube.
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
    std::uint64_t lastCoverSig = 0;

    std::vector<util::SimpleIdentity> drawableIds;

    // GLBs baked into map geometry, cached per model id.
    std::map<std::string, model::BakedModel> meshCache;

    // Shared soft contact-shadow texture (built lazily).
    gfx::Texture2DPtr shadowTexture;
};

} // namespace mbgl
