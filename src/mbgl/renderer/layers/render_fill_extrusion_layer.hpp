#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_impl.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_properties.hpp>

#include <memory>

namespace mbgl {

class ShadowPass;

class RenderFillExtrusionLayer final : public RenderLayer {
public:
    explicit RenderFillExtrusionLayer(Immutable<style::FillExtrusionLayer::Impl>);
    ~RenderFillExtrusionLayer() override;

#if MLN_RENDER_BACKEND_METAL
    /// Hand this layer the renderer-owned shared ShadowPass before update(). The layer registers its
    /// caster/receiver drawables into the shared pass instead of owning a per-layer ShadowMap. Set by
    /// RenderOrchestrator under the Metal + shadowsEnabled gate; null otherwise (stock FE path).
    void setShadowPass(ShadowPass* pass) { shadowPass = pass; }
#endif

private:
    void transition(const TransitionParameters&) override;
    void evaluate(const PropertyEvaluationParameters&) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;
    bool is3D() const override;

    /// Generate any changes needed by the layer
    void update(gfx::ShaderRegistry&,
                gfx::Context&,
                const TransformState&,
                const std::shared_ptr<UpdateParameters>&,
                const RenderTree&,
                UniqueChangeRequestVec&) override;

    bool queryIntersectsFeature(const GeometryCoordinates&,
                                const GeometryTileFeature&,
                                float,
                                const TransformState&,
                                float,
                                const mat4&,
                                const FeatureState&) const override;

    // Paint properties
    style::FillExtrusionPaintProperties::Unevaluated unevaluated;

    gfx::ShaderGroupPtr fillExtrusionGroup;
    gfx::ShaderGroupPtr fillExtrusionPatternGroup;

#if MLN_RENDER_BACKEND_METAL
    // Directional-shadow path (S1, iOS/Metal-only). Gated at runtime by the 3D-enhancements
    // flag; when off, none of this is created and the stock FE path is used unchanged.
    void markLayerRenderable(bool willRender, UniqueChangeRequestVec&) override;
    void layerRemoved(UniqueChangeRequestVec&) override;
    void layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec&) override;
    std::size_t removeTile(RenderPass, const OverscaledTileID&) override;
    std::size_t removeAllDrawables() override;
    // The shared shadow map + per-frame light frustum live on the renderer-owned ShadowPass (set
    // via setShadowPass). This layer owns only its receiver/caster tweakers + its ground group.
    ShadowPass* shadowPass = nullptr;
    TileLayerGroupPtr groundShadowLayerGroup;
    gfx::ShaderGroupPtr fillExtrusionShadowGroup;
    gfx::ShaderGroupPtr groundShadowGroup;
    gfx::ShaderGroupPtr shadowDepthGroup;
    // Strong ref — the caster group stores only a weak_ptr (runTweakers drops expired ones).
    LayerTweakerPtr shadowCasterTweaker;
    LayerTweakerPtr groundShadowTweaker;
#endif

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    gfx::ShaderGroupPtr fillExtrusionInstancedGroup;
    gfx::ShaderGroupPtr fillExtrusionPatternInstancedGroup;

    using FillExtrusionVertexVector = gfx::VertexVector<FillExtrusionStaticVertex>;
    using TriangleIndexVector = gfx::IndexVector<gfx::Triangles>;

    std::shared_ptr<FillExtrusionVertexVector> staticDataVertices;
    std::shared_ptr<TriangleIndexVector> staticDataIndices;
    std::shared_ptr<SegmentVector> staticDataSegments;
#endif
};

} // namespace mbgl
