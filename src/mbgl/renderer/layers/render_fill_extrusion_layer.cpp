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

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/shadows/shadow_pass.hpp>
#include <mbgl/renderer/shadows/shadow_tweakers.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <cstdlib>
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

// Shadow runtime gate (shadowsEnabled / shadowMapSize) lives in shadow_pass.{hpp,cpp} now — the
// single source of truth shared by the renderer-owned ShadowPass and this layer.

} // namespace

RenderFillExtrusionLayer::RenderFillExtrusionLayer(Immutable<style::FillExtrusionLayer::Impl> _impl)
    : RenderLayer(makeMutable<FillExtrusionLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()) {
    styleDependencies = unevaluated.getDependencies();
}

RenderFillExtrusionLayer::~RenderFillExtrusionLayer() = default;

#if MLN_RENDER_BACKEND_METAL
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
        shadowPass->releaseCasterGroup(getID());
        shadowCasterGroup = nullptr;
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
    // This layer's own caster group in the shared pass (cached in update()); pruning it never
    // touches another layer's casters.
    if (shadowCasterGroup) {
        stats.drawablesRemoved += shadowCasterGroup->removeDrawables(RenderPass::Opaque, tileID).size();
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
    // Per-layer caster group (keyed {layerID,tileID} in the pass registry): clearing it evicts only
    // this layer's casters, never another fill-extrusion layer's.
    if (shadowCasterGroup) {
        stats.drawablesRemoved += shadowCasterGroup->getDrawableCount();
        shadowCasterGroup->clearDrawables();
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
#if MLN_RENDER_BACKEND_METAL
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

    bool useShadows = false;
#if MLN_RENDER_BACKEND_METAL
    // The shared shadow map + per-frame light frustum are owned by RenderOrchestrator's ShadowPass
    // (handed to this layer via setShadowPass() before update()). This layer no longer owns a
    // ShadowMap or registers the shadow RenderTarget — it only registers caster/receiver drawables
    // into the shared pass and samples the shared texture.
    useShadows = shadowsEnabled() && shadowPass && shadowPass->ready();
    const ShadowFrustumStatePtr frustumState = useShadows ? shadowPass->frustumState()
                                                          : ShadowFrustumStatePtr{};
    if (useShadows) {
        if (!shadowDepthGroup) {
            shadowDepthGroup = shaders.getShaderGroup("ShadowDepthShader");
        }
        if (!fillExtrusionShadowGroup) {
            fillExtrusionShadowGroup = shaders.getShaderGroup("FillExtrusionShadowShader");
        }
        if (!groundShadowGroup) {
            groundShadowGroup = shaders.getShaderGroup("GroundShadowShader");
        }
        // This layer's own slot in the shared caster registry (keyed {layerID,tileID}). Cached so
        // the lifecycle removeTile/removeAllDrawables can prune it without a gfx::Context.
        shadowCasterGroup = shadowPass->casterGroupFor(context, getID());
        // Caster tweaker on this layer's caster group; it writes each caster's light_matrix from
        // that drawable's own tileID, so one tweaker serves every caster in the group.
        if (shadowCasterGroup && !shadowCasterTweaker) {
            shadowCasterTweaker = std::make_shared<ShadowDepthTweaker>(
                getID(), evaluatedProperties, shadowMapSize(), frustumState);
            shadowCasterGroup->addLayerTweaker(shadowCasterTweaker);
        }
    } else {
        shadowCasterGroup = nullptr;
    }

    // Ground cast shadows default ON (gated by the master useShadows above); MLN_GROUND_SHADOWS=0
    // force-disables (e.g. to isolate building-only shadows). Only the orchestrator-designated
    // ground OWNER (lowest fill-extrusion layer) draws ground quads — ground-once, so overlapping
    // highlight layers never stack a second darkening pass over the same building.
    const char* groundShadowEnv = std::getenv("MLN_GROUND_SHADOWS");
    const bool useGroundShadows = useShadows && shadowGroundOwner && groundShadowGroup &&
                                  !(groundShadowEnv && std::string_view(groundShadowEnv) == "0");
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

    if (!layerTweaker) {
#if MLN_RENDER_BACKEND_METAL
        if (useShadows) {
            layerTweaker = std::make_shared<FillExtrusionShadowTweaker>(
                getID(), evaluatedProperties, shadowMapSize(), frustumState);
        } else
#endif
        {
            layerTweaker = std::make_shared<FillExtrusionLayerTweaker>(getID(), evaluatedProperties);
        }
        layerGroup->addLayerTweaker(layerTweaker);
    }

    if (!fillExtrusionGroup) {
        fillExtrusionGroup = shaders.getShaderGroup("FillExtrusionShader");
    }
    if (!fillExtrusionPatternGroup) {
        fillExtrusionPatternGroup = shaders.getShaderGroup("FillExtrusionPatternShader");
    }
#if MLN_RENDER_BACKEND_METAL
    // Non-pattern buildings receive shadows: use the shadow-receiving shader group + texture.
    if (useShadows && fillExtrusionShadowGroup) {
        fillExtrusionGroup = fillExtrusionShadowGroup;
    }
#endif

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
#if MLN_RENDER_BACKEND_METAL
    if (groundShadowLayerGroup) {
        stats.drawablesRemoved += groundShadowLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return !(drawable.getRenderPass() & drawPass) || (tileID && !hasRenderTile(*tileID));
        });
    }
    if (shadowCasterGroup) {
        stats.drawablesRemoved += shadowCasterGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return !(drawable.getRenderPass() & RenderPass::Opaque) || (tileID && !hasRenderTile(*tileID));
        });
    }
#endif

    const auto layerPrefix = getID() + "/";
    const auto hasPattern = !unevaluated.get<FillExtrusionPattern>().isUndefined();
    const auto opaque = evaluated.get<FillExtrusionOpacity>() >= 1;

    std::unique_ptr<gfx::DrawableBuilder> depthBuilder;
    std::unique_ptr<gfx::DrawableBuilder> colorBuilder;

    const auto& shaderGroup = hasPattern ? fillExtrusionPatternGroup : fillExtrusionGroup;
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
#if MLN_RENDER_BACKEND_METAL
        bool missingShadowSidecar = false;
        if (useGroundShadows && groundShadowLayerGroup &&
            groundShadowLayerGroup->getDrawableCount(drawPass, tileID) == 0) {
            missingShadowSidecar = true;
        }
        if (useShadows && shadowDepthGroup && bucket.sharedTriangles->elements()) {
            if (shadowCasterGroup && shadowCasterGroup->getDrawableCount(RenderPass::Opaque, tileID) == 0) {
                missingShadowSidecar = true;
            }
        }
        if (missingShadowSidecar) {
            removeTile(drawPass, tileID);
        }
#endif
        if (updateTile(drawPass, tileID, std::move(updateExisting))) {
            continue;
        }

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

        auto instanceAttrs = context.createVertexAttributeArray();
        instanceAttrs->readDataDrivenPaintProperties<FillExtrusionBase,
                                                     FillExtrusionColor,
                                                     FillExtrusionHeight,
                                                     FillExtrusionPattern>(
            binders, evaluated, instancePropertiesAsUniforms, idFillExtrusionBaseVertexAttribute);

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
#if MLN_RENDER_BACKEND_METAL
                if (useShadows) {
                    builder->setTexture(shadowPass->texture(), idFillExtrusionShadowTexture);
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
#if MLN_RENDER_BACKEND_METAL
                if (useShadows) {
                    builder->setTexture(shadowPass->texture(), idFillExtrusionShadowTexture);
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

#if MLN_RENDER_BACKEND_METAL
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
        if (doDepthPass) {
            finish(*depthBuilder);
        }
        finish(*colorBuilder);

#if MLN_RENDER_BACKEND_METAL
        // Env toggle (diagnostic): MLN_GROUND_SHADOWS=1 enables the ground-shadow quads.
        if (useGroundShadows && groundShadowLayerGroup) {
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
                    groundBuilder->setTexture(shadowPass->texture(), idGroundShadowTexture);
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
        if (useShadows && shadowDepthGroup && casterAttrs && bucket.sharedTriangles->elements()) {
            if (auto* casterGroup = shadowCasterGroup) {
                if (const auto casterShader = std::static_pointer_cast<gfx::ShaderProgramBase>(
                        shadowDepthGroup->getOrCreateShader(context, propertiesAsUniforms))) {
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
                        // MLN_SHADOW_CULL (debug): 0=none 1=front 2=back(default).
                        gfx::CullFaceMode casterCull{.enabled = true,
                                                     .side = gfx::CullFaceSideType::Back,
                                                     .winding = gfx::CullFaceWindingType::CounterClockwise};
                        if (const char* cm = std::getenv("MLN_SHADOW_CULL")) {
                            const int v = std::atoi(cm);
                            if (v == 0) casterCull.enabled = false;
                            else if (v == 1) casterCull.side = gfx::CullFaceSideType::Front;
                        }
                        casterBuilder->setCullFaceMode(casterCull);
                        casterBuilder->setRawVertices({}, vertexCount, gfx::AttributeDataType::Short2);
                        casterBuilder->setVertexAttributes(std::move(casterAttrs));
                        casterBuilder->setSegments(gfx::Triangles(),
                                                   bucket.sharedTriangles,
                                                   bucket.triangleSegments.data(),
                                                   bucket.triangleSegments.size());
                        casterBuilder->flush(context);
                        for (auto& drawable : casterBuilder->clearDrawables()) {
                            drawable->setTileID(tileID);
                            drawable->setLayerTweaker(shadowCasterTweaker);
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
        if (doDepthPass) {
            finishInstance(*instancedDepthBuilder);
        }
        finishInstance(*instancedColorBuilder);
#endif
    }
}

} // namespace mbgl
