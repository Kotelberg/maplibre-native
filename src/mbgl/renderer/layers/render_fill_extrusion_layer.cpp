#include <mbgl/renderer/layers/render_fill_extrusion_layer.hpp>

#include <mbgl/geometry/feature_index.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/renderer/image_manager.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/tile_render_data.hpp>
#include <mbgl/style/expression/image.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_impl.hpp>
#include <mbgl/tile/geometry_tile.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/util/intersection_tests.hpp>
#include <mbgl/util/math.hpp>

#include <mbgl/gfx/drawable_atlases_tweaker.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/fill_extrusion_layer_tweaker.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/fill_extrusion_layer_ubo.hpp>
#include <mbgl/shaders/shader_program_base.hpp>
#include <mbgl/renderer/shadows/shadow_support.hpp>

#if MLN_DRAWABLE_SHADOWS
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/shadows/shadow_pass.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>
#endif

namespace mbgl {

using namespace style;
using namespace shaders;

namespace {

inline const FillExtrusionLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == FillExtrusionLayer::Impl::staticTypeInfo());
    return static_cast<const FillExtrusionLayer::Impl&>(*impl);
}

// shadowMapSize() lives in shadow_pass.{hpp,cpp} — the single source of truth shared by the
// renderer-owned ShadowPass and this layer.

} // namespace

RenderFillExtrusionLayer::RenderFillExtrusionLayer(Immutable<style::FillExtrusionLayer::Impl> _impl)
    : RenderLayer(makeMutable<FillExtrusionLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()) {
    styleDependencies = unevaluated.getDependencies();
}

RenderFillExtrusionLayer::~RenderFillExtrusionLayer() = default;

#if MLN_DRAWABLE_SHADOWS
void RenderFillExtrusionLayer::markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) {
    isRenderable = willRender;

    activateLayerGroup(groundShadowLayerGroup, willRender, changes);
    activateLayerGroup(layerGroup, willRender, changes);
    // The shared shadow-map RenderTarget is owned + (un)registered by RenderOrchestrator (it is
    // shared across all fill-extrusion layers), so per-layer lifecycle must NOT touch it.
}

void RenderFillExtrusionLayer::layerRemoved(UniqueChangeRequestVec& changes) {
    removeAllDrawables();
    activateLayerGroup(groundShadowLayerGroup, false, changes);
    activateLayerGroup(layerGroup, false, changes);
    // Drop this layer's slot in the shared caster registry; the shadow target itself is owned by
    // RenderOrchestrator (see markLayerRenderable).
    if (shadowPass) {
        shadowPass->releaseCasterGroup(getID()); // removes this layer's group on every cascade
        shadowCasterGroups.clear();
    }
}

void RenderFillExtrusionLayer::layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec& changes) {
    layerIndex = newLayerIndex;
    changeLayerIndex(groundShadowLayerGroup, newLayerIndex, changes);
    changeLayerIndex(layerGroup, newLayerIndex, changes);
}

std::size_t RenderFillExtrusionLayer::removeTile(RenderPass renderPass, const OverscaledTileID& tileID) {
    const auto oldValue = stats.drawablesRemoved;
    if (const auto tileGroup = static_cast<TileLayerGroup*>(layerGroup.get())) {
        stats.drawablesRemoved += tileGroup->removeDrawables(renderPass, tileID).size();
    }
    if (groundShadowLayerGroup) {
        stats.drawablesRemoved += groundShadowLayerGroup->removeDrawables(renderPass, tileID).size();
    }
    // This layer's own per-cascade caster groups in the shared pass (cached in update()); pruning
    // them never touches another layer's casters.
    for (auto* casterGroup : shadowCasterGroups) {
        if (casterGroup) {
            stats.drawablesRemoved += casterGroup->removeDrawables(RenderPass::Opaque, tileID).size();
        }
    }
    return stats.drawablesRemoved - oldValue;
}

std::size_t RenderFillExtrusionLayer::removeAllDrawables() {
    const auto oldValue = stats.drawablesRemoved;
    if (layerGroup) {
        stats.drawablesRemoved += layerGroup->getDrawableCount();
        layerGroup->clearDrawables();
    }
    if (groundShadowLayerGroup) {
        stats.drawablesRemoved += groundShadowLayerGroup->getDrawableCount();
        groundShadowLayerGroup->clearDrawables();
    }
    // Per-layer per-cascade caster groups (keyed {layerID,cascade} in the pass registry): clearing
    // them evicts only this layer's casters, never another fill-extrusion layer's.
    for (auto* casterGroup : shadowCasterGroups) {
        if (casterGroup) {
            stats.drawablesRemoved += casterGroup->getDrawableCount();
            casterGroup->clearDrawables();
        }
    }
    return stats.drawablesRemoved - oldValue;
}
#endif

void RenderFillExtrusionLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
    styleDependencies = unevaluated.getDependencies();
}

void RenderFillExtrusionLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    const auto previousProperties = staticImmutableCast<FillExtrusionLayerProperties>(evaluatedProperties);
    auto properties = makeMutable<FillExtrusionLayerProperties>(
        staticImmutableCast<FillExtrusionLayer::Impl>(baseImpl),
        parameters.getCrossfadeParameters(),
        unevaluated.evaluate(parameters, previousProperties->evaluated));

    passes = (properties->evaluated.get<style::FillExtrusionOpacity>() > 0) ? RenderPass::Translucent
                                                                            : RenderPass::None;
    properties->renderPasses = mbgl::underlying_type(passes);
    evaluatedProperties = std::move(properties);

    if (layerTweaker) {
        layerTweaker->updateProperties(evaluatedProperties);
    }
#if MLN_DRAWABLE_SHADOWS
    if (groundShadowTweaker) {
        groundShadowTweaker->updateProperties(evaluatedProperties);
    }
#endif
}

bool RenderFillExtrusionLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderFillExtrusionLayer::hasCrossfade() const {
    return getCrossfade<FillExtrusionLayerProperties>(evaluatedProperties).t != 1;
}

bool RenderFillExtrusionLayer::is3D() const {
    return true;
}

bool RenderFillExtrusionLayer::queryIntersectsFeature(const GeometryCoordinates& queryGeometry,
                                                      const GeometryTileFeature& feature,
                                                      const float,
                                                      const TransformState& transformState,
                                                      const float pixelsToTileUnits,
                                                      const mat4&,
                                                      const FeatureState&) const {
    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;
    auto translatedQueryGeometry = FeatureIndex::translateQueryGeometry(
        queryGeometry,
        evaluated.get<style::FillExtrusionTranslate>(),
        evaluated.get<style::FillExtrusionTranslateAnchor>(),
        static_cast<float>(transformState.getBearing()),
        pixelsToTileUnits);

    return util::polygonIntersectsMultiPolygon(translatedQueryGeometry.value_or(queryGeometry),
                                               feature.getGeometries());
}

void RenderFillExtrusionLayer::update(gfx::ShaderRegistry& shaders,
                                      gfx::Context& context,
                                      const TransformState&,
                                      const std::shared_ptr<UpdateParameters>&,
                                      const RenderTree&,
                                      UniqueChangeRequestVec& changes) {
    if (!renderTiles || renderTiles->empty() || passes == RenderPass::None) {
        removeAllDrawables();
        return;
    }

    [[maybe_unused]] bool useShadows = false; // all reads are Metal-gated; unused on other backends
#if MLN_DRAWABLE_SHADOWS
    // The shared shadow map + per-frame light frustum are owned by RenderOrchestrator's ShadowPass
    // (handed to this layer via setShadowPass() before update()). This layer no longer owns a
    // ShadowMap or registers the shadow RenderTarget — it only registers caster/receiver drawables
    // into the shared pass and samples the shared texture.
    useShadows = shadowPass && shadowPass->ready();
    const ShadowFrustumStatePtr frustumState = useShadows ? shadowPass->frustumState()
                                                          : ShadowFrustumStatePtr{};
    if (useShadows) {
        if (!shadowDepthGroup) {
            shadowDepthGroup = shaders.getShaderGroup("ShadowDepthShader");
        }
#if MLN_USE_FILL_EXTRUSION_INSTANCING
        if (!shadowDepthInstancedGroup) {
            shadowDepthInstancedGroup = shaders.getShaderGroup("ShadowDepthInstancedShader");
        }
#endif
        if (!fillExtrusionShadowGroup) {
            fillExtrusionShadowGroup = shaders.getShaderGroup("FillExtrusionShadowShader");
        }
        if (!groundShadowGroup) {
            groundShadowGroup = shaders.getShaderGroup("GroundShadowShader");
        }
        // This layer's per-cascade slots in the shared caster registry (keyed {layerID,cascade}).
        // Cached so the lifecycle removeTile/removeAllDrawables can prune them without a gfx::Context.
        // One caster group + tweaker per cascade; each tweaker is bound to its cascade index so it
        // writes that cascade's light_matrix (the near map gets the tight frustum, the far the full).
        const uint32_t cascadeCount = shadowPass->cascadeCount();
        shadowCasterGroups.assign(cascadeCount, nullptr);
        if (shadowCasterTweakers.size() != cascadeCount) {
            shadowCasterTweakers.assign(cascadeCount, nullptr);
        }
        for (uint32_t c = 0; c < cascadeCount; ++c) {
            shadowCasterGroups[c] = shadowPass->casterGroupFor(context, getID(), c);
            if (shadowCasterGroups[c] && !shadowCasterTweakers[c]) {
                shadowCasterTweakers[c] = std::make_shared<ShadowDepthTweaker>(
                    getID(), evaluatedProperties, shadowMapSize(), frustumState, c);
                shadowCasterGroups[c]->addLayerTweaker(shadowCasterTweakers[c]);
            }
        }
    } else {
        shadowCasterGroups.clear();
    }

    // Ground cast shadows are part of "cast shadows" being on (gated by the master useShadows above).
    // Only the orchestrator-designated ground OWNER (lowest fill-extrusion layer) draws ground quads
    // — ground-once, so higher layers never stack a second darkening pass over the same building.
    const bool useGroundShadows = useShadows && shadowGroundOwner && groundShadowGroup;
    if (useGroundShadows) {
        if (!groundShadowLayerGroup) {
            groundShadowLayerGroup =
                context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID() + "-ground-shadow");
            if (groundShadowLayerGroup) {
                activateLayerGroup(groundShadowLayerGroup, isRenderable, changes);
            }
        }
        if (groundShadowLayerGroup && !groundShadowTweaker) {
            groundShadowTweaker =
                std::make_shared<GroundShadowTweaker>(getID(), evaluatedProperties, shadowMapSize(), frustumState);
            groundShadowLayerGroup->addLayerTweaker(groundShadowTweaker);
        }
    } else if (groundShadowLayerGroup) {
        stats.drawablesRemoved += groundShadowLayerGroup->clearDrawables();
        activateLayerGroup(groundShadowLayerGroup, false, changes);
        groundShadowLayerGroup.reset();
        groundShadowTweaker.reset();
    }
#endif

    // Set up the building layer group after the ground-shadow group so equal layer indexes
    // render ground shadows first and buildings second.
    if (!layerGroup) {
        if (auto layerGroup_ = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID())) {
            setLayerGroup(std::move(layerGroup_), changes);
        } else {
            return;
        }
    }

    const auto hasPattern = !unevaluated.get<FillExtrusionPattern>().isUndefined();
#if MLN_DRAWABLE_SHADOWS
    // ShadowPass can become ready after this layer's first update. The plain
    // and shadow receiver tweakers populate different UBO layouts, so changing
    // only the shader leaves later rebuilt roof drawables reading invalid data.
    // Replacing the tweaker changes its pointer identity; updateTile() below
    // then removes and rebuilds every stale visible drawable for this mode.
    const bool useShadowReceiver = useShadows && !hasPattern && fillExtrusionShadowGroup;
    if (layerTweaker && receiverUsesShadows != useShadowReceiver) {
        layerTweaker.reset();
    }
#endif

    if (!layerTweaker) {
#if MLN_DRAWABLE_SHADOWS
        if (useShadowReceiver) {
            layerTweaker = std::make_shared<FillExtrusionShadowTweaker>(
                getID(), evaluatedProperties, shadowMapSize(), frustumState);
        } else
#endif
        {
            layerTweaker = std::make_shared<FillExtrusionLayerTweaker>(getID(), evaluatedProperties);
        }
#if MLN_DRAWABLE_SHADOWS
        receiverUsesShadows = useShadowReceiver;
#endif
        layerGroup->addLayerTweaker(layerTweaker);
    }

    if (!fillExtrusionGroup) {
        fillExtrusionGroup = shaders.getShaderGroup("FillExtrusionShader");
    }
    if (!fillExtrusionPatternGroup) {
        fillExtrusionPatternGroup = shaders.getShaderGroup("FillExtrusionPatternShader");
    }
    auto* tileLayerGroup = static_cast<TileLayerGroup*>(layerGroup.get());

    const auto& evaluated = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties).evaluated;

    constexpr auto drawPass = RenderPass::Translucent;

    stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
        // If the render pass has changed or the tile has  dropped out of the cover set, remove it.
        const auto& tileID = drawable.getTileID();
        if (!(drawable.getRenderPass() & drawPass) || (tileID && !hasRenderTile(*tileID))) {
            return true;
        }
        return false;
    });
#if MLN_DRAWABLE_SHADOWS
    if (groundShadowLayerGroup) {
        stats.drawablesRemoved += groundShadowLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return !(drawable.getRenderPass() & drawPass) || (tileID && !hasRenderTile(*tileID));
        });
    }
    for (auto* casterGroup : shadowCasterGroups) {
        if (casterGroup) {
            stats.drawablesRemoved += casterGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                const auto& tileID = drawable.getTileID();
                return !(drawable.getRenderPass() & RenderPass::Opaque) || (tileID && !hasRenderTile(*tileID));
            });
        }
    }
#endif

    const auto layerPrefix = getID() + "/";
    const auto opaque = evaluated.get<FillExtrusionOpacity>() >= 1;

    std::unique_ptr<gfx::DrawableBuilder> depthBuilder;
    std::unique_ptr<gfx::DrawableBuilder> colorBuilder;

#if MLN_DRAWABLE_SHADOWS
    // Keep fillExtrusionGroup permanently plain; selecting a local active group
    // makes the shader reversible and keeps it aligned with the tweaker above.
    const auto& shaderGroup = hasPattern          ? fillExtrusionPatternGroup
                              : useShadowReceiver ? fillExtrusionShadowGroup
                                                  : fillExtrusionGroup;
#else
    const auto& shaderGroup = hasPattern ? fillExtrusionPatternGroup : fillExtrusionGroup;
#endif
    if (!shaderGroup) {
        removeAllDrawables();
        return;
    }

    tileLayerGroup->setStencilTiles(renderTiles);

#if MLN_USE_FILL_EXTRUSION_INSTANCING
    if (!fillExtrusionInstancedGroup) {
        fillExtrusionInstancedGroup = shaders.getShaderGroup("FillExtrusionInstancedShader");
    }
    if (!fillExtrusionPatternInstancedGroup) {
        fillExtrusionPatternInstancedGroup = shaders.getShaderGroup("FillExtrusionPatternInstancedShader");
    }

    if (!staticDataVertices) {
        staticDataVertices = std::make_shared<FillExtrusionVertexVector>(RenderStaticData::fillExtrusionVertices());
    }
    if (!staticDataIndices) {
        staticDataIndices = std::make_shared<TriangleIndexVector>(RenderStaticData::fillExtrusionTriangleIndices());
    }
    if (!staticDataSegments) {
        staticDataSegments = std::make_shared<SegmentVector>(RenderStaticData::fillExtrusionSegments());
    }

    const auto& instancedShaderGroup = hasPattern ? fillExtrusionPatternInstancedGroup : fillExtrusionInstancedGroup;
    if (!instancedShaderGroup) {
        removeAllDrawables();
        return;
    }

    const auto instanceVertexCount = staticDataVertices->elements();
    std::unique_ptr<gfx::DrawableBuilder> instancedDepthBuilder;
    std::unique_ptr<gfx::DrawableBuilder> instancedColorBuilder;
    StringIDSetsPair instancePropertiesAsUniforms;
#endif

    StringIDSetsPair propertiesAsUniforms;
    for (const RenderTile& tile : *renderTiles) {
        const auto& tileID = tile.getOverscaledTileID();

        const auto* optRenderData = getRenderDataForPass(tile, drawPass);
        if (!optRenderData || !optRenderData->bucket || !optRenderData->bucket->hasData()) {
            removeTile(drawPass, tileID);
            continue;
        }

        const auto& renderData = *optRenderData;
        auto& bucket = static_cast<FillExtrusionBucket&>(*renderData.bucket);

        const auto prevBucketID = getRenderTileBucketID(tileID);
        if (prevBucketID != util::SimpleIdentity::Empty && prevBucketID != bucket.getID()) {
            // This tile was previously set up from a different bucket, drop and re-create any drawables for it.
            removeTile(drawPass, tileID);
        }
        setRenderTileBucketID(tileID, bucket.getID());

        gfx::DrawableTweakerPtr tweaker;
        if (depthBuilder) {
            depthBuilder->clearTweakers();
        }
        if (colorBuilder) {
            colorBuilder->clearTweakers();
        }

        const auto vertexCount = bucket.vertices.elements();
        auto& binders = bucket.paintPropertyBinders.at(getID());

        // If we already have drawables for this tile, update them.
        auto updateExisting = [&](gfx::Drawable& drawable) {
            if (drawable.getLayerTweaker() != layerTweaker) {
                // This drawable was produced on a previous style/bucket, and should not be updated.
                return false;
            }
            return true;
        };

        // Does the VISIBLE building drawable (roof + instanced walls) already exist for this tile and
        // match the current style? updateTile returns true when it does (and prunes stale ones). This
        // is now evaluated BEFORE any shadow-sidecar bookkeeping so that a missing sidecar can no
        // longer force the visible drawable to be destroyed and re-uploaded.
        const bool visiblePresent = updateTile(drawPass, tileID, std::move(updateExisting));

#if MLN_DRAWABLE_SHADOWS
        // Detect a missing shadow SIDECAR (the ground-shadow quad and/or the per-cascade casters)
        // WITHOUT touching the visible drawable. The old code removeTile'd the visible building to force
        // a full rebuild whenever a sidecar flapped — e.g. the ground-shadow owner being reassigned
        // between fill-extrusion layers during a pan — re-uploading every building's geometry on each
        // such frame (the "visible tile rebuild spikes" hotspot). Now the visible drawable is left
        // intact and ONLY the missing sidecar piece is refilled below; its cost is a fraction of the
        // roof+wall rebuild.
        bool missingGroundSidecar = false;
        bool missingCasterSidecar = false;
        if (useGroundShadows && groundShadowLayerGroup &&
            groundShadowLayerGroup->getDrawableCount(drawPass, tileID) == 0) {
            missingGroundSidecar = true;
        }
        if (useShadows && shadowDepthGroup && bucket.sharedTriangles->elements()) {
            // If ANY cascade is missing this tile's casters, refill all (the caster build refills every
            // cascade together, so a partial set means the whole caster sidecar must be regenerated).
            for (auto* casterGroup : shadowCasterGroups) {
                if (casterGroup && casterGroup->getDrawableCount(RenderPass::Opaque, tileID) == 0) {
                    missingCasterSidecar = true;
                    break;
                }
            }
        }
        if (visiblePresent && !missingGroundSidecar && !missingCasterSidecar) {
            continue; // visible drawable + every shadow sidecar already current — nothing to rebuild
        }
        // A partial caster set (some cascades have this tile, some don't) would duplicate on refill, so
        // clear the tile from every caster group first and let the build below repopulate them cleanly.
        if (visiblePresent && missingCasterSidecar) {
            for (auto* casterGroup : shadowCasterGroups) {
                if (casterGroup) {
                    stats.drawablesRemoved += casterGroup->removeDrawables(RenderPass::Opaque, tileID).size();
                }
            }
        }
        // Which pieces to (re)build this iteration. When the visible drawable is intact we skip the
        // expensive roof/wall rebuild and refill only the missing sidecar piece(s).
        const bool buildVisible = !visiblePresent;
        const bool buildGround = !visiblePresent || missingGroundSidecar;
        const bool buildCasters = !visiblePresent || missingCasterSidecar;
#else
        if (visiblePresent) {
            continue;
        }
        constexpr bool buildVisible = true;
#endif

        propertiesAsUniforms.first.clear();
        propertiesAsUniforms.second.clear();

        auto vertexAttrs = context.createVertexAttributeArray();
        vertexAttrs->readDataDrivenPaintProperties<FillExtrusionBase,
                                                   FillExtrusionColor,
                                                   FillExtrusionHeight,
                                                   FillExtrusionPattern>(
            binders, evaluated, propertiesAsUniforms, idFillExtrusionBaseVertexAttribute);

        const auto shader = std::static_pointer_cast<gfx::ShaderProgramBase>(
            shaderGroup->getOrCreateShader(context, propertiesAsUniforms));
        if (!shader) {
            continue;
        }

#if MLN_USE_FILL_EXTRUSION_INSTANCING
        if (instancedDepthBuilder) {
            instancedDepthBuilder->clearTweakers();
        }
        if (instancedColorBuilder) {
            instancedColorBuilder->clearTweakers();
        }

        instancePropertiesAsUniforms.first.clear();
        instancePropertiesAsUniforms.second.clear();

        // Read the data-driven base/height/color (and pattern) as per-instance attributes. MUST
        // happen before any builder flush() below — flush clears the binder vertex data, after which
        // a later read would mark these as uniforms (zero-height walls). Mirrors the Metal ordering.
        auto instanceAttrs = context.createVertexAttributeArray();
        instanceAttrs->readDataDrivenPaintProperties<FillExtrusionBase,
                                                     FillExtrusionColor,
                                                     FillExtrusionHeight,
                                                     FillExtrusionPattern>(
            binders, evaluated, instancePropertiesAsUniforms, idFillExtrusionBaseVertexAttribute);


#if MLN_USE_FILL_EXTRUSION_INSTANCING
#if MLN_DRAWABLE_SHADOWS
        // Pre-build the instanced WALL caster's attributes HERE, before the color/roof-caster builders
        // flush() below — flush uploads + clears the binder vertex data, after which
        // readDataDrivenPaintProperties marks data-driven props (base/height) as UNIFORM
        // (getVertexCount()==0 path), zeroing the caster's per-instance height → zero-height walls cast
        // nothing. Reading now captures the (shared) binder buffers; the caster is drawn further below.
        StringIDSetsPair casterInstanceUniforms;
        gfx::VertexAttributeArrayPtr casterStaticVertices;
        gfx::VertexAttributeArrayPtr casterInstanceAttrs;
        if (useShadows && shadowDepthInstancedGroup) {
            casterStaticVertices = context.createVertexAttributeArray();
            if (const auto& attr = casterStaticVertices->set(idFillExtrusionPosVertexAttribute)) {
                attr->setSharedRawData(staticDataVertices,
                                       offsetof(FillExtrusionStaticVertex, a1),
                                       /*vertexOffset=*/0,
                                       sizeof(FillExtrusionStaticVertex),
                                       gfx::AttributeDataType::Short2);
            }
            casterInstanceAttrs = context.createVertexAttributeArray();
            casterInstanceAttrs->readDataDrivenPaintProperties<FillExtrusionBase,
                                                               FillExtrusionColor,
                                                               FillExtrusionHeight,
                                                               FillExtrusionPattern>(
                binders, evaluated, casterInstanceUniforms, idFillExtrusionBaseVertexAttribute);
            // The wall caster consumes base/height in a separate shader variant from the visible
            // instanced walls. Copy those two float2 streams out of the shared paint byte buffer so
            // Metal gives them independent instance layouts instead of reusing the interleaved binder
            // buffer that was built for the visible FillExtrusionInstancedShader.
            const auto deinterleaveFloat2 = [&](const std::size_t id) {
                const auto& sharedAttr = casterInstanceAttrs->get(id);
                if (!sharedAttr || !sharedAttr->getSharedRawData()) {
                    return;
                }
                const auto raw = sharedAttr->getSharedRawData();
                const auto offset = sharedAttr->getSharedOffset();
                const auto stride = sharedAttr->getSharedStride();
                const auto instanceCount = bucket.sharedVertices->elements();
                const auto* bytes = static_cast<const std::byte*>(raw->getRawData());
                const auto& copiedAttr =
                    casterInstanceAttrs->set(id, -1, gfx::AttributeDataType::Float2, instanceCount);
                if (!copiedAttr) {
                    return;
                }
                std::vector<std::uint8_t> data(instanceCount * sizeof(gfx::VertexAttribute::float2));
                for (std::size_t i = 0; i < instanceCount; ++i) {
                    gfx::VertexAttribute::float2 value;
                    std::memcpy(&value, bytes + i * stride + offset, sizeof(value));
                    std::memcpy(data.data() + i * sizeof(value), &value, sizeof(value));
                }
                copiedAttr->setRawData(std::move(data));
                copiedAttr->setStride(sizeof(gfx::VertexAttribute::float2));
            };
            deinterleaveFloat2(idFillExtrusionBaseVertexAttribute);
            deinterleaveFloat2(idFillExtrusionHeightVertexAttribute);
            if (const auto& attr = casterInstanceAttrs->set(idFillExtrusionOutlinePosAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(FillExtrusionLayoutVertex, a1),
                                       /*vertexOffset=*/0,
                                       sizeof(FillExtrusionLayoutVertex),
                                       gfx::AttributeDataType::Short2);
            }
            if (const auto& attr = casterInstanceAttrs->set(idFillExtrusionEdDiscardAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(FillExtrusionLayoutVertex, a2),
                                       /*vertexOffset=*/0,
                                       sizeof(FillExtrusionLayoutVertex),
                                       gfx::AttributeDataType::UShort2);
            }
        }
#endif // MLN_DRAWABLE_SHADOWS
#endif // MLN_USE_FILL_EXTRUSION_INSTANCING (shadow caster prebuild is Metal-only)


        const auto instancedShader = std::static_pointer_cast<gfx::ShaderProgramBase>(
            instancedShaderGroup->getOrCreateShader(context, instancePropertiesAsUniforms));
        if (!instancedShader) {
            continue;
        }
#endif

        // The non-pattern path in `render()` only uses two-pass rendering if there's translucency.
        // The pattern path always uses two passes.
        const auto doDepthPass = (!opaque || hasPattern);

        if (doDepthPass && !depthBuilder) {
            if (auto builder = context.createDrawableBuilder(layerPrefix + "depth")) {
                builder->setShader(shader);
                builder->setIs3D(true);
                builder->setEnableColor(false);
                builder->setRenderPass(drawPass);
                builder->setCullFaceMode(gfx::CullFaceMode::backCCW());
                builder->setDrawPriority(0);
                if (tweaker) {
                    builder->addTweaker(tweaker);
                }
#if MLN_DRAWABLE_SHADOWS
                if (useShadowReceiver) {
                    // Bind one shadow texture per cascade; the receiver shader picks the tightest
                    // cascade that contains the fragment (idFillExtrusionShadowTexture0 + cascade).
                    // Bind ALL kMaxShadowCascades slots the shader declares (Metal needs every
                    // declared texture argument bound); slots past the active count reuse the far
                    // cascade as a harmless dummy (never sampled — the shader loops to cascade_count).
                    for (uint32_t c = 0; c < kMaxShadowCascades; ++c) {
                        const uint32_t src = std::min(c, shadowPass->cascadeCount() - 1u);
                        builder->setTexture(shadowPass->texture(src), idFillExtrusionShadowTexture0 + c);
                    }
                }
#endif
                depthBuilder = std::move(builder);
            }
        }
        if (!colorBuilder) {
            if (auto builder = context.createDrawableBuilder(layerPrefix + "color")) {
                builder->setShader(shader);
                builder->setIs3D(true);
                builder->setEnableColor(true);
                builder->setColorMode(gfx::ColorMode::alphaBlended());
                builder->setRenderPass(drawPass);
                builder->setCullFaceMode(gfx::CullFaceMode::backCCW());
                builder->setDrawPriority(1);
                if (tweaker) {
                    builder->addTweaker(tweaker);
                }
#if MLN_DRAWABLE_SHADOWS
                if (useShadowReceiver) {
                    // Bind one shadow texture per cascade; the receiver shader picks the tightest
                    // cascade that contains the fragment (idFillExtrusionShadowTexture0 + cascade).
                    // Bind ALL kMaxShadowCascades slots the shader declares (Metal needs every
                    // declared texture argument bound); slots past the active count reuse the far
                    // cascade as a harmless dummy (never sampled — the shader loops to cascade_count).
                    for (uint32_t c = 0; c < kMaxShadowCascades; ++c) {
                        const uint32_t src = std::min(c, shadowPass->cascadeCount() - 1u);
                        builder->setTexture(shadowPass->texture(src), idFillExtrusionShadowTexture0 + c);
                    }
                }
#endif
                colorBuilder = std::move(builder);
            }
        }

        if (hasPattern && !tweaker) {
            if (const auto& atlases = tile.getAtlasTextures()) {
                tweaker = std::make_shared<gfx::DrawableAtlasesTweaker>(atlases,
                                                                        std::nullopt,
                                                                        idFillExtrusionImageTexture,
                                                                        /*isText=*/false,
                                                                        false,
                                                                        style::AlignmentType::Auto,
                                                                        false,
                                                                        false);
                if (depthBuilder) {
                    depthBuilder->addTweaker(tweaker);
                }
                if (colorBuilder) {
                    colorBuilder->addTweaker(tweaker);
                }

#if MLN_USE_FILL_EXTRUSION_INSTANCING
                if (instancedDepthBuilder) {
                    instancedDepthBuilder->addTweaker(tweaker);
                }
                if (instancedColorBuilder) {
                    instancedColorBuilder->addTweaker(tweaker);
                }
#endif
            }
        }

        if (const auto& attr = vertexAttrs->set(idFillExtrusionPosVertexAttribute)) {
            attr->setSharedRawData(bucket.sharedVertices,
                                   offsetof(FillExtrusionLayoutVertex, a1),
                                   /*vertexOffset=*/0,
                                   sizeof(FillExtrusionLayoutVertex),
                                   gfx::AttributeDataType::Short2);
        }
#if !MLN_USE_FILL_EXTRUSION_INSTANCING
        if (const auto& attr = vertexAttrs->set(idFillExtrusionNormalEdVertexAttribute)) {
            attr->setSharedRawData(bucket.sharedVertices,
                                   offsetof(FillExtrusionLayoutVertex, a2),
                                   /*vertexOffset=*/0,
                                   sizeof(FillExtrusionLayoutVertex),
                                   gfx::AttributeDataType::Short4);
        }
#endif

        if (doDepthPass) {
            depthBuilder->setRawVertices({}, vertexCount, gfx::AttributeDataType::Short2);
            depthBuilder->setVertexAttributes(vertexAttrs);
        }

#if MLN_DRAWABLE_SHADOWS
        // Keep a reference to the (shared) vertex attributes for the shadow caster before
        // the color builder takes ownership.
        auto casterAttrs = useShadows ? vertexAttrs : decltype(vertexAttrs){};
#endif

        colorBuilder->setEnableStencil(doDepthPass);
        colorBuilder->setRawVertices({}, vertexCount, gfx::AttributeDataType::Short2);
        colorBuilder->setVertexAttributes(std::move(vertexAttrs));

        const auto finish = [&](gfx::DrawableBuilder& builder) {
            if (!bucket.sharedTriangles->elements()) {
                return;
            }
            builder.setSegments(gfx::Triangles(),
                                bucket.sharedTriangles,
                                bucket.triangleSegments.data(),
                                bucket.triangleSegments.size());

            builder.flush(context);

            for (auto& drawable : builder.clearDrawables()) {
                drawable->setTileID(tileID);
                drawable->setType(static_cast<std::size_t>(hasPattern));
                drawable->setLayerTweaker(layerTweaker);
                drawable->setBinders(renderData.bucket, &binders);
                drawable->setRenderTile(renderTilesOwner, &tile);

                tileLayerGroup->addDrawable(drawPass, tileID, std::move(drawable));
                ++stats.drawablesAdded;
            }
        };
        // Visible roof drawables: only when the visible drawable is being (re)built. When we are here
        // solely to refill a missing shadow sidecar, the existing roof/walls are kept (buildVisible=0).
        if (buildVisible) {
            if (doDepthPass) {
                finish(*depthBuilder);
            }
            finish(*colorBuilder);
        }

#if MLN_DRAWABLE_SHADOWS
        // Ground-shadow quads (drawn by the ground owner only).
        if (buildGround && useGroundShadows && groundShadowLayerGroup) {
            if (const auto groundShader = std::static_pointer_cast<gfx::ShaderProgramBase>(
                    groundShadowGroup->getOrCreateShader(context, {}))) {
                if (auto groundBuilder = context.createDrawableBuilder(layerPrefix + "groundShadow")) {
                    groundBuilder->setShader(groundShader);
                    // Isolated in its own group: the quad is a pure alpha-blend ground overlay
                    // drawn before the building group, while buildings keep their 3D state.
                    groundBuilder->setIs3D(true);
                    groundBuilder->setEnableColor(true);
                    groundBuilder->setEnableDepth(false);
                    groundBuilder->setEnableStencil(false);
                    groundBuilder->setColorMode(gfx::ColorMode::alphaBlended());
                    groundBuilder->setCullFaceMode(gfx::CullFaceMode::disabled());
                    groundBuilder->setRenderPass(drawPass);
                    groundBuilder->setDrawPriority(-1);
                    groundBuilder->setVertexAttrId(idGroundShadowPosVertexAttribute);
                    for (uint32_t c = 0; c < kMaxShadowCascades; ++c) {
                        const uint32_t src = std::min(c, shadowPass->cascadeCount() - 1u);
                        groundBuilder->setTexture(shadowPass->texture(src), idGroundShadowTexture0 + c);
                    }
                    groundBuilder->addQuad(0, 0, util::EXTENT, util::EXTENT);
                    groundBuilder->flush(context);
                    for (auto& drawable : groundBuilder->clearDrawables()) {
                        drawable->setTileID(tileID);
                        drawable->setLayerTweaker(groundShadowTweaker);
                        drawable->setRenderTile(renderTilesOwner, &tile);

                        groundShadowLayerGroup->addDrawable(drawPass, tileID, std::move(drawable));
                        ++stats.drawablesAdded;
                    }
                }
            }
        }

        // Build a depth-only caster drawable from the same geometry into this layer's caster group
        // in the shared shadow map (rendered from the sun's POV by the shadow RenderTarget).
        if (buildCasters && useShadows && shadowDepthGroup && casterAttrs && bucket.sharedTriangles->elements()) {
            if (const auto casterShader = std::static_pointer_cast<gfx::ShaderProgramBase>(
                    shadowDepthGroup->getOrCreateShader(context, propertiesAsUniforms))) {
                // One caster drawable per cascade — each renders into its cascade's shadow map with
                // that cascade's frustum (shadowCasterTweakers[c]). The vertex buffers + segments are
                // shared GPU data; the attribute handle is copied per cascade, not deep-duplicated.
                for (uint32_t c = 0; c < shadowCasterGroups.size(); ++c) {
                    auto* casterGroup = shadowCasterGroups[c];
                    if (!casterGroup) {
                        continue;
                    }
                    if (auto casterBuilder = context.createDrawableBuilder(layerPrefix + "shadowCaster")) {
                        casterBuilder->setShader(casterShader);
                        // is3D + enableDepth route the caster through mtl::TileLayerGroup's
                        // group-level depthModeFor3D() (LessEqual + write) against the shadow
                        // target's Float32 depth attachment (ShadowMap withDepth=true). Drawable::draw
                        // skips depth-stencil setup for is3D, so the layer group owns it; with no
                        // stencil enabled it takes the depth-only state (the target has no stencil).
                        // Result: the nearest-to-light caster's packed depth survives = real occlusion.
                        casterBuilder->setIs3D(true);
                        casterBuilder->setEnableColor(true);
                        casterBuilder->setColorMode(gfx::ColorMode::unblended()); // write packed depth (replace)
                        casterBuilder->setEnableDepth(true);
                        casterBuilder->setRenderPass(RenderPass::Opaque);
                        // Render FRONT faces (cull back) into the shadow map: the nearest-to-light
                        // surface — the ROOF and sun-facing walls — is the true occluder. The
                        // earlier front-cull (second-depth technique) assumes a CLOSED mesh; fill-
                        // extrusion buildings are OPEN (walls + roof, no floor), so culling front
                        // dropped the roof and left only thin far-wall slivers → thin, edge-tracing
                        // ground shadows. Rendering the roof gives solid, correctly-sized cast
                        // shadows; receiver-side shadow_bias handles the mild self-shadow risk.
                        const gfx::CullFaceMode casterCull{.enabled = true,
                                                           .side = gfx::CullFaceSideType::Back,
                                                           .winding = gfx::CullFaceWindingType::CounterClockwise};
                        casterBuilder->setCullFaceMode(casterCull);
                        casterBuilder->setRawVertices({}, vertexCount, gfx::AttributeDataType::Short2);
                        // Copy the shared attribute handle per cascade (all cascades reference the
                        // same underlying GPU vertex buffer; the handle is just ref-counted).
                        auto cascadeAttrs = casterAttrs;
                        casterBuilder->setVertexAttributes(std::move(cascadeAttrs));
                        casterBuilder->setSegments(gfx::Triangles(),
                                                   bucket.sharedTriangles,
                                                   bucket.triangleSegments.data(),
                                                   bucket.triangleSegments.size());
                        casterBuilder->flush(context);
                        for (auto& drawable : casterBuilder->clearDrawables()) {
                            drawable->setTileID(tileID);
                            drawable->setLayerTweaker(shadowCasterTweakers[c]);
                            drawable->setBinders(renderData.bucket, &binders);
                            drawable->setRenderTile(renderTilesOwner, &tile);
                            casterGroup->addDrawable(RenderPass::Opaque, tileID, std::move(drawable));
                            ++stats.drawablesAdded;
                        }
                    }
                }
            }
        }
#endif

#if MLN_USE_FILL_EXTRUSION_INSTANCING
        if (doDepthPass && !instancedDepthBuilder) {
            if (auto builder = context.createDrawableBuilder(layerPrefix + "depthInstanced")) {
                builder->setShader(instancedShader);
                builder->setIs3D(true);
                builder->setEnableColor(false);
                builder->setRenderPass(drawPass);
                builder->setCullFaceMode(gfx::CullFaceMode::backCCW());
                builder->setDrawPriority(0);
                if (tweaker) {
                    builder->addTweaker(tweaker);
                }
                instancedDepthBuilder = std::move(builder);
            }
        }
        if (!instancedColorBuilder) {
            if (auto builder = context.createDrawableBuilder(layerPrefix + "colorInstanced")) {
                builder->setShader(instancedShader);
                builder->setIs3D(true);
                builder->setEnableColor(true);
                builder->setColorMode(gfx::ColorMode::alphaBlended());
                builder->setRenderPass(drawPass);
                builder->setCullFaceMode(gfx::CullFaceMode::backCCW());
                builder->setDrawPriority(1);
                if (tweaker) {
                    builder->addTweaker(tweaker);
                }
                instancedColorBuilder = std::move(builder);
            }
        }

        auto instanceVertexAttrs = context.createVertexAttributeArray();
        if (const auto& attr = instanceVertexAttrs->set(idFillExtrusionPosVertexAttribute)) {
            attr->setSharedRawData(staticDataVertices,
                                   offsetof(FillExtrusionStaticVertex, a1),
                                   /*vertexOffset=*/0,
                                   sizeof(FillExtrusionStaticVertex),
                                   gfx::AttributeDataType::Short2);
        }
#if MLN_USE_FILL_EXTRUSION_INSTANCING
        // Metal indexes the outline buffer by gl_InstanceID in-shader, so it binds the raw outline
        // pos + ed_discard here. The GL path already bound its edge-indexed geometry (pos0/pos1/
        // normal0/normal1/edgedistance) onto instanceAttrs in the early block above.
        if (const auto& attr = instanceAttrs->set(idFillExtrusionOutlinePosAttribute)) {
            attr->setSharedRawData(bucket.sharedVertices,
                                   offsetof(FillExtrusionLayoutVertex, a1),
                                   /*vertexOffset=*/0,
                                   sizeof(FillExtrusionLayoutVertex),
                                   gfx::AttributeDataType::Short2);
        }
        if (const auto& attr = instanceAttrs->set(idFillExtrusionEdDiscardAttribute)) {
            attr->setSharedRawData(bucket.sharedVertices,
                                   offsetof(FillExtrusionLayoutVertex, a2),
                                   /*vertexOffset=*/0,
                                   sizeof(FillExtrusionLayoutVertex),
                                   gfx::AttributeDataType::UShort2);
        }
#endif

        if (doDepthPass) {
            instancedDepthBuilder->setRawVertices({}, instanceVertexCount, gfx::AttributeDataType::Short2);
            instancedDepthBuilder->setVertexAttributes(instanceVertexAttrs);
            instancedDepthBuilder->setInstanceAttributes(instanceAttrs);
        }

        instancedColorBuilder->setEnableStencil(doDepthPass);
        instancedColorBuilder->setRawVertices({}, instanceVertexCount, gfx::AttributeDataType::Short2);
        instancedColorBuilder->setVertexAttributes(std::move(instanceVertexAttrs));
        instancedColorBuilder->setInstanceAttributes(std::move(instanceAttrs));

        const auto finishInstance = [&](gfx::DrawableBuilder& instancedBuilder) {
            if (!staticDataIndices->elements()) {
                return;
            }
            instancedBuilder.setSegments(
                gfx::Triangles(), staticDataIndices, staticDataSegments->data(), staticDataSegments->size());

            instancedBuilder.flush(context);

            for (auto& drawable : instancedBuilder.clearDrawables()) {
                drawable->setTileID(tileID);
                drawable->setType(static_cast<std::size_t>(hasPattern));
                drawable->setLayerTweaker(layerTweaker);
                drawable->setBinders(renderData.bucket, &binders);
                drawable->setRenderTile(renderTilesOwner, &tile);

                tileLayerGroup->addDrawable(drawPass, tileID, std::move(drawable));
                ++stats.drawablesAdded;
            }
        };
        if (buildVisible) {
            if (doDepthPass) {
                finishInstance(*instancedDepthBuilder);
            }
            finishInstance(*instancedColorBuilder);
        }

#if MLN_USE_FILL_EXTRUSION_INSTANCING && MLN_DRAWABLE_SHADOWS
        // Instanced WALL caster (Metal): the roof-only sharedTriangles caster (above) leaves ground
        // shadows detached from the base on the instanced path, because the walls never enter the shadow
        // map. Cast the walls too — same per-edge OutlineInstance geometry as the visible instanced walls,
        // via the ShadowDepthInstancedShader (light-space depth). One drawable per cascade (each gets that
        // cascade's light_matrix from shadowCasterTweakers[c]). Depth-only, so cull is disabled (both wall
        // faces occlude). Together roof+wall casters fill the full building volume → shadows reattach.
        // (The GL instanced wall caster is wired in Task 11.)
        if (buildCasters && useShadows && shadowDepthInstancedGroup && casterInstanceAttrs && casterStaticVertices &&
            !shadowCasterGroups.empty() && bucket.sharedVertices->elements() && staticDataIndices->elements()) {
            if (const auto instCasterShader = std::static_pointer_cast<gfx::ShaderProgramBase>(
                    shadowDepthInstancedGroup->getOrCreateShader(context, casterInstanceUniforms))) {
                for (uint32_t c = 0; c < shadowCasterGroups.size(); ++c) {
                    auto* casterGroup = shadowCasterGroups[c];
                    if (!casterGroup) {
                        continue;
                    }
                    if (auto wallCaster = context.createDrawableBuilder(layerPrefix + "shadowCasterWall")) {
                        wallCaster->setShader(instCasterShader);
                        wallCaster->setIs3D(true);
                        wallCaster->setEnableColor(true);
                        wallCaster->setColorMode(gfx::ColorMode::unblended()); // write packed depth (replace)
                        wallCaster->setEnableDepth(true);
                        wallCaster->setRenderPass(RenderPass::Opaque);
                        wallCaster->setCullFaceMode(gfx::CullFaceMode::disabled()); // depth-only; both faces
                        wallCaster->setRawVertices({}, instanceVertexCount, gfx::AttributeDataType::Short2);
                        // Copy the shared attribute handles per cascade (shared GPU buffers; handles
                        // are ref-counted, like the roof caster above).
                        auto cascadeStatic = casterStaticVertices;
                        auto cascadeInstance = casterInstanceAttrs;
                        wallCaster->setVertexAttributes(std::move(cascadeStatic));
                        wallCaster->setInstanceAttributes(std::move(cascadeInstance));
                        wallCaster->setSegments(gfx::Triangles(),
                                                staticDataIndices,
                                                staticDataSegments->data(),
                                                staticDataSegments->size());
                        wallCaster->flush(context);
                        for (auto& drawable : wallCaster->clearDrawables()) {
                            drawable->setTileID(tileID);
                            drawable->setLayerTweaker(shadowCasterTweakers[c]);
                            drawable->setBinders(renderData.bucket, &binders);
                            drawable->setRenderTile(renderTilesOwner, &tile);
                            casterGroup->addDrawable(RenderPass::Opaque, tileID, std::move(drawable));
                            ++stats.drawablesAdded;
                        }
                    }
                }
            }
        }
#endif // MLN_USE_FILL_EXTRUSION_INSTANCING && MLN_DRAWABLE_SHADOWS

#endif
    }
}

} // namespace mbgl
