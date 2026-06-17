#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_impl.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_properties.hpp>
#include <mbgl/renderer/shadows/shadow_support.hpp>

#include <memory>
#include <vector>

namespace mbgl {

class ShadowPass;

class RenderFillExtrusionLayer final : public RenderLayer {
public:
    explicit RenderFillExtrusionLayer(Immutable<style::FillExtrusionLayer::Impl>);
    ~RenderFillExtrusionLayer() override;

#if MLN_DRAWABLE_SHADOWS
    /// Hand this layer the renderer-owned shared ShadowPass before update(). The layer registers its
    /// caster/receiver drawables into the shared pass instead of owning a per-layer ShadowMap. Set by
    /// RenderOrchestrator under the Metal + shadowsEnabled gate; null otherwise (stock FE path).
    void setShadowPass(ShadowPass* pass) { shadowPass = pass; }

    /// Designate this layer as the single ground-shadow owner (ground-once). The orchestrator marks
    /// the lowest fill-extrusion layer that has render tiles as owner; only the owner draws the z=0
    /// ground-shadow quads, so highlight layers (hover/listings/selected) cast into the shared map
    /// but never stack a second darkening ground draw over the same building.
    void setShadowGroundOwner(bool v) { shadowGroundOwner = v; }

    /// Whether this layer currently has render tiles — used by the orchestrator to skip a tile-less
    /// lowest layer when picking the ground owner (#31: avoids a 1-frame ground-shadow dropout when
    /// the base layer is momentarily tile-less while a higher layer casts).
    bool hasRenderTiles() const { return renderTiles && !renderTiles->empty(); }
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

#if MLN_DRAWABLE_SHADOWS
    // Directional-shadow path (S1, iOS/Metal-only). Gated at runtime by the 3D-enhancements
    // flag; when off, none of this is created and the stock FE path is used unchanged.
    void markLayerRenderable(bool willRender, UniqueChangeRequestVec&) override;
    void layerRemoved(UniqueChangeRequestVec&) override;
    void layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec&) override;
    std::size_t removeTile(RenderPass, const OverscaledTileID&) override;
    std::size_t removeAllDrawables() override;
    // The shared shadow map + per-frame light frustum live on the renderer-owned ShadowPass (set
    // via setShadowPass). This layer owns only its receiver/caster tweakers + (when ground owner)
    // its ground group. `shadowCasterGroup` caches this layer's slot in the pass's caster registry
    // (owned by the pass; cached here so the lifecycle removeTile/removeAllDrawables can prune it
    // without a gfx::Context). `shadowGroundOwner` gates the single ground draw (set by orchestrator).
    ShadowPass* shadowPass = nullptr;
    // One caster group per cascade (near→far), each a cached slot in the pass's {layerID,cascade}
    // registry (owned by the pass; cached so removeTile/removeAllDrawables can prune without a
    // gfx::Context). Empty when shadows are inactive.
    std::vector<TileLayerGroup*> shadowCasterGroups;
    bool shadowGroundOwner = false;
    TileLayerGroupPtr groundShadowLayerGroup;
    gfx::ShaderGroupPtr fillExtrusionShadowGroup;
    gfx::ShaderGroupPtr groundShadowGroup;
    gfx::ShaderGroupPtr shadowDepthGroup;
#if MLN_USE_FILL_EXTRUSION_INSTANCING
    // Instanced WALL caster shader. The roof-only sharedTriangles caster (shadowDepthGroup) leaves ground
    // shadows detached from the base on the instanced path; this casts the walls so they reattach.
    gfx::ShaderGroupPtr shadowDepthInstancedGroup;
#endif
    // Strong refs (one per cascade) — the caster groups store only weak_ptrs (runTweakers drops
    // expired ones). Each tweaker is bound to its cascade index so it writes that cascade's matrix.
    std::vector<LayerTweakerPtr> shadowCasterTweakers;
    LayerTweakerPtr groundShadowTweaker;
#endif

#if MLN_USE_FILL_EXTRUSION_INSTANCING || MLN_GL_FE_INSTANCING
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
