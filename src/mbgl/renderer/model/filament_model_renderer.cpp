#if MLN_WITH_FILAMENT_MODELS

#include <mbgl/renderer/model/filament_model_renderer.hpp>

#include <mbgl/util/logging.hpp>
#include <mbgl/util/projection.hpp>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <gltfio/materials/uberarchive.h>

#include <math/mat4.h>
#include <math/vec3.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <cmath>
#include <fstream>

namespace mbgl {
namespace model {

namespace fl = filament;

namespace {

struct LoadedModel {
    fl::gltfio::FilamentAsset* asset = nullptr;
    std::vector<fl::gltfio::FilamentInstance*> instances; // grows on demand
    float meshHeight = 1.0f;
    fl::math::float3 centerToBase{0, 0, 0};
};

} // namespace

struct FilamentModelRenderer::Backend {
    fl::Engine* engine = nullptr;
    fl::Renderer* renderer = nullptr;
    fl::Scene* scene = nullptr;
    fl::View* view = nullptr;
    fl::Camera* camera = nullptr;
    fl::SwapChain* swapChain = nullptr;
    uint32_t swapW = 0, swapH = 0;

    fl::gltfio::AssetLoader* loader = nullptr;
    fl::gltfio::MaterialProvider* materials = nullptr;
    fl::gltfio::TextureProvider* stbProvider = nullptr;
    std::unique_ptr<fl::gltfio::ResourceLoader> resourceLoader;

    std::map<std::string, LoadedModel> models;

    bool initialize() {
        if (engine) return true;
#if defined(__APPLE__)
        engine = fl::Engine::create(fl::backend::Backend::METAL);
#else
        engine = fl::Engine::create(fl::backend::Backend::OPENGL);
#endif
        if (!engine) return false;
        renderer = engine->createRenderer();
        scene = engine->createScene();
        view = engine->createView();

        utils::Entity camEntity = utils::EntityManager::get().create();
        camera = engine->createCamera(camEntity);
        view->setScene(scene);
        view->setCamera(camera);
        view->setBlendMode(fl::View::BlendMode::TRANSLUCENT);
        renderer->setClearOptions({.clearColor = {0.0, 0.0, 0.0, 0.0}, .clear = true});

        materials = fl::gltfio::createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        loader = fl::gltfio::AssetLoader::create({engine, materials});

        fl::gltfio::ResourceConfiguration rc{};
        rc.engine = engine;
        rc.gltfPath = ".";
        rc.normalizeSkinningWeights = true;
        resourceLoader = std::make_unique<fl::gltfio::ResourceLoader>(rc);
        stbProvider = fl::gltfio::createStbProvider(engine);
        resourceLoader->addTextureProvider("image/png", stbProvider);
        resourceLoader->addTextureProvider("image/jpeg", stbProvider);

        utils::Entity sun = utils::EntityManager::get().create();
        fl::LightManager::Builder(fl::LightManager::Type::DIRECTIONAL)
            .color({0.98f, 0.92f, 0.89f})
            .intensity(120000.0f)
            .direction(fl::math::float3{-0.45f, -0.35f, -0.8f})
            .castShadows(false)
            .build(*engine, sun);
        scene->addEntity(sun);

        // Soft fill from the opposite side so back facades aren't pitch black
        utils::Entity fill = utils::EntityManager::get().create();
        fl::LightManager::Builder(fl::LightManager::Type::DIRECTIONAL)
            .color({0.9f, 0.93f, 1.0f})
            .intensity(40000.0f)
            .direction(fl::math::float3{0.5f, 0.4f, -0.6f})
            .castShadows(false)
            .build(*engine, fill);
        scene->addEntity(fill);
        return true;
    }

    LoadedModel* loadModel(const std::string& id, const std::string& path) {
        auto it = models.find(id);
        if (it != models.end()) return it->second.asset ? &it->second : nullptr;

        LoadedModel& m = models[id]; // inserted even on failure → negative cache
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            Log::Warning(Event::General, "FilamentModelRenderer: cannot open " + path);
            return nullptr;
        }
        std::vector<uint8_t> glb((std::istreambuf_iterator<char>(in)), {});
        // Instanced asset so further instances can be added on demand
        // (plain createAsset caps at one instance).
        fl::gltfio::FilamentInstance* first = nullptr;
        auto* asset = loader->createInstancedAsset(glb.data(), static_cast<uint32_t>(glb.size()), &first, 1);
        if (!asset || !first) {
            Log::Warning(Event::General, "FilamentModelRenderer: createInstancedAsset failed for " + path);
            return nullptr;
        }
        resourceLoader->loadResources(asset);
        // NOTE: source data intentionally NOT released — AssetLoader::createInstance
        // needs it when the instance pool grows lazily.

        const auto aabb = asset->getBoundingBox();
        const auto center = aabb.center();
        const auto extent = aabb.extent();
        m.asset = asset;
        m.meshHeight = std::max(extent.y * 2.0f, 1e-6f);
        m.centerToBase = fl::math::float3{-center.x, -(center.y - extent.y), -center.z};
        m.instances.push_back(first);
        scene->addEntities(first->getEntities(), first->getEntityCount());
        return &m;
    }

    fl::gltfio::FilamentInstance* instanceAt(LoadedModel& m, size_t index) {
        while (m.instances.size() <= index) {
            auto* inst = loader->createInstance(m.asset);
            if (!inst) return nullptr;
            resourceLoader->loadResources(m.asset);
            scene->addEntities(inst->getEntities(), inst->getEntityCount());
            m.instances.push_back(inst);
        }
        return m.instances[index];
    }
};

FilamentModelRenderer::FilamentModelRenderer()
    : backend(std::make_unique<Backend>()) {}

FilamentModelRenderer::~FilamentModelRenderer() {
    if (backend && backend->engine) {
        // Engine::destroy tears down all owned objects.
        fl::Engine::destroy(&backend->engine);
    }
}

void FilamentModelRenderer::setAssets(std::map<std::string, std::string> idToPath) {
    assets = std::move(idToPath);
}

std::shared_ptr<PremultipliedImage> FilamentModelRenderer::render(const std::vector<ModelInstanceSpec>& specs,
                                                                  const mat4& proj,
                                                                  double anchorWorldX,
                                                                  double anchorWorldY,
                                                                  double worldSize,
                                                                  double zoom,
                                                                  uint32_t width,
                                                                  uint32_t height,
                                                                  const CropRect* crop) {
    if (!backend->initialize()) return nullptr;
    auto& b = *backend;

    if (!b.swapChain || b.swapW != width || b.swapH != height) {
        if (b.swapChain) b.engine->destroy(b.swapChain);
        b.swapChain = b.engine->createSwapChain(width, height);
        b.swapW = width;
        b.swapH = height;
    }
    b.view->setViewport({0, 0, width, height});

    // Camera: clipFlipY × P_map × translate(anchor), all double precision.
    // (clipFlipY restores right-handed winding — the map's projected world is
    // left-handed and Filament does not compensate mirrored custom projections;
    // the upside-down framebuffer cancels the GL-style bottom-up readback.)
    fl::math::mat4 P(proj[0], proj[1], proj[2], proj[3],
                     proj[4], proj[5], proj[6], proj[7],
                     proj[8], proj[9], proj[10], proj[11],
                     proj[12], proj[13], proj[14], proj[15]);
    const fl::math::mat4 anchor = fl::math::mat4::translation(fl::math::double3{anchorWorldX, anchorWorldY, 0.0});
    const fl::math::mat4 clipFlipY(fl::math::double4{1, 0, 0, 0},
                                   fl::math::double4{0, -1, 0, 0},
                                   fl::math::double4{0, 0, 1, 0},
                                   fl::math::double4{0, 0, 0, 1});
    fl::math::mat4 full = clipFlipY * P * anchor;
    if (crop) {
        // Linear clip-space crop: x' = x/hx - (cx/hx)·w (valid pre-divide).
        fl::math::mat4 C(fl::math::double4{1.0 / crop->hx, 0, 0, 0},
                         fl::math::double4{0, 1.0 / crop->hy, 0, 0},
                         fl::math::double4{0, 0, 1, 0},
                         fl::math::double4{-crop->cx / crop->hx, -crop->cy / crop->hy, 0, 1});
        full = clipFlipY * C * P * anchor;
    }
    b.camera->setCustomProjection(full, 0.1, 100000.0);
    b.camera->setModelMatrix(fl::math::mat4f{});

    // Per-instance transforms, fp32-safe relative to the anchor.
    std::map<std::string, size_t> instanceCounters;
    auto& tcm = b.engine->getTransformManager();
    for (const auto& spec : specs) {
        const auto assetIt = assets.find(spec.modelId);
        if (assetIt == assets.end()) continue;
        LoadedModel* m = b.loadModel(spec.modelId, assetIt->second);
        if (!m) continue;
        auto* inst = b.instanceAt(*m, instanceCounters[spec.modelId]++);
        if (!inst) continue;

        const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(spec.latitude, zoom);
        const double k = spec.sizeMeters / m->meshHeight; // mesh units → meters
        const double sxy = k / metersPerPixel;            // meters → world px (x/y)
        const double dx = spec.worldFractionX * worldSize - anchorWorldX;
        const double dy = spec.worldFractionY * worldSize - anchorWorldY;

        const auto root = tcm.getInstance(inst->getRoot());
        const fl::math::mat4f local =
            fl::math::mat4f::translation(fl::math::float3{(float)dx, (float)dy, 0.0f}) *
            fl::math::mat4f::rotation((float)(spec.yawDegrees * M_PI / 180.0), fl::math::float3{0, 0, 1}) *
            fl::math::mat4f::scaling(fl::math::float3{(float)sxy, (float)sxy, (float)k}) *
            fl::math::mat4f::rotation((float)M_PI_2, fl::math::float3{1, 0, 0}) *
            fl::math::mat4f::translation(m->centerToBase);
        tcm.setTransform(root, local);
    }

    // Hide unused pooled instances by collapsing them to zero scale.
    for (auto& [id, m] : b.models) {
        const size_t used = instanceCounters.count(id) ? instanceCounters[id] : 0;
        for (size_t i = used; i < m.instances.size(); ++i) {
            tcm.setTransform(tcm.getInstance(m.instances[i]->getRoot()), fl::math::mat4f::scaling(0.0f));
        }
    }

    auto image = std::make_shared<PremultipliedImage>(Size(width, height));
    bool done = false;
    bool issued = false;
    for (int frame = 0; frame < 60 && !done; ++frame) {
        if (b.renderer->beginFrame(b.swapChain)) {
            b.renderer->render(b.view);
            if (frame >= 2 && !issued) {
                issued = true;
                fl::backend::PixelBufferDescriptor pbd(
                    image->data.get(),
                    image->bytes(),
                    fl::backend::PixelDataFormat::RGBA,
                    fl::backend::PixelDataType::UBYTE,
                    [](void*, size_t, void* user) { *static_cast<bool*>(user) = true; },
                    &done);
                b.renderer->readPixels(0, 0, width, height, std::move(pbd));
            }
            b.renderer->endFrame();
        }
        b.engine->flushAndWait();
    }
    if (!done) {
        Log::Warning(Event::General, "FilamentModelRenderer: readPixels did not complete");
        return nullptr;
    }
    return image;
}

} // namespace model
} // namespace mbgl

#endif // MLN_WITH_FILAMENT_MODELS
