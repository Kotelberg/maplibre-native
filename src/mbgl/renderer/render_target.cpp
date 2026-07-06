#include <mbgl/renderer/render_target.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/util/image.hpp>

#include <cstdlib>
#include <fstream>

namespace mbgl {

RenderTarget::RenderTarget(gfx::Context& context_,
                           const Size size,
                           const gfx::TextureChannelDataType type,
                           bool withDepth_)
    : context(context_),
      withDepth(withDepth_) {
    offscreenTexture = context.createOffscreenTexture(size, type, withDepth_, /*stencil=*/false);
}

RenderTarget::~RenderTarget() {}

const gfx::Texture2DPtr& RenderTarget::getTexture() {
    return offscreenTexture->getTexture();
};

bool RenderTarget::addLayerGroup(LayerGroupBasePtr layerGroup, const bool replace) {
    const auto index = layerGroup->getLayerIndex();
    const auto result = layerGroupsByLayerIndex.insert(std::make_pair(index, LayerGroupBasePtr{}));
    if (result.second) {
        // added
        result.first->second = std::move(layerGroup);
        return true;
    } else {
        // not added
        if (replace) {
            result.first->second = std::move(layerGroup);
            return true;
        } else {
            return false;
        }
    }
}

bool RenderTarget::removeLayerGroup(const int32_t layerIndex) {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    if (hit != layerGroupsByLayerIndex.end()) {
        layerGroupsByLayerIndex.erase(hit);
        return true;
    } else {
        return false;
    }
}

size_t RenderTarget::numLayerGroups() const noexcept {
    return layerGroupsByLayerIndex.size();
}

static const LayerGroupBasePtr no_group;

const LayerGroupBasePtr& RenderTarget::getLayerGroup(const int32_t layerIndex) const {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    return (hit == layerGroupsByLayerIndex.end()) ? no_group : hit->second;
}

void RenderTarget::upload(gfx::UploadPass& uploadPass) {
    visitLayerGroups(([&](LayerGroupBase& layerGroup) { layerGroup.upload(uploadPass); }));
}

void RenderTarget::render(RenderOrchestrator& orchestrator, const RenderTree& renderTree, PaintParameters& parameters) {
    parameters.renderPass = parameters.encoder->createRenderPass(
        "render target",
        {.renderable = *offscreenTexture,
         // Depth-capable targets (shadow maps) clear color to white == packed-far depth
         // and clear depth to 1.0 so the nearest caster wins; color-only targets keep black.
         .clearColor = withDepth ? Color{1.0f, 1.0f, 1.0f, 1.0f} : Color{0.0f, 0.0f, 0.0f, 1.0f},
         .clearDepth = withDepth ? std::optional<float>(1.0f) : std::optional<float>{},
         .clearStencil = {}});

    const gfx::ScissorRect prevScissorRect = parameters.scissorRect;
    const auto& size = getTexture()->getSize();
    parameters.scissorRect = {.x = 0, .y = 0, .width = size.width, .height = size.height};

    // Run layer tweakers to update any dynamic elements
    parameters.currentLayer = 0;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.runTweakers(renderTree, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, opaque pass
    parameters.pass = RenderPass::Opaque;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = 0;
    visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, translucent pass
    parameters.pass = RenderPass::Translucent;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = static_cast<uint32_t>(numLayerGroups()) - 1;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        if (parameters.currentLayer > 0) {
            parameters.currentLayer--;
        }
    });

    parameters.renderPass.reset();
    parameters.encoder->present(*offscreenTexture);

    parameters.scissorRect = prevScissorRect;

#ifndef NDEBUG
    // MLN_SHADOW_DUMP (debug-only diagnostic; compiled out of release/opt builds, env-gated within
    // debug builds so no effect on the byte-identical off path): dump a depth-capable render target's
    // RGBA8 (packed-depth) texture to a PNG so caster-depth coverage is observable in headless
    // mbgl-render. Fires only when MLN_SHADOW_DUMP names a file prefix AND this target owns a depth
    // attachment (the shadow map). Effect: writes <prefix>_<n>.png per frame; default off.
    if (withDepth) {
        if (const char* prefix = std::getenv("MLN_SHADOW_DUMP")) {
            static int dumpCounter = 0;
            const PremultipliedImage img = offscreenTexture->readStillImage();
            // Quick coverage probe: count non-(255,255,255) pixels (cleared white == far == no caster).
            size_t covered = 0;
            const size_t npx = static_cast<size_t>(img.size.width) * img.size.height;
            for (size_t i = 0; i < npx; ++i) {
                const uint8_t* p = img.data.get() + i * 4;
                if (!(p[0] == 255 && p[1] == 255 && p[2] == 255)) ++covered;
            }
            const std::string png = encodePNG(img);
            const std::string path = std::string(prefix) + "_" + std::to_string(dumpCounter) + ".png";
            std::ofstream out(path, std::ios::binary);
            out.write(png.data(), static_cast<std::streamsize>(png.size()));
            fprintf(stderr, "MLN_SHADOW_DUMP frame=%d coverage=%.2f%% -> %s\n",
                    dumpCounter, 100.0 * static_cast<double>(covered) / static_cast<double>(npx), path.c_str());
            ++dumpCounter;
        }
    }
#endif
}

} // namespace mbgl
