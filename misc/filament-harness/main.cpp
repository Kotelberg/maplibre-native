// M2 Filament harness: headless Metal rendering of a GLB, optionally with a
// MapLibre camera (see docs/superpowers/plans/2026-06-12-maplibre-model-layer-m2.md).
//
// Modes:
//   filament-harness                         → clear-color render, /tmp/harness-clear.ppm
//   filament-harness --glb Duck.glb          → fixed lookAt camera, /tmp/harness-duck.ppm
//   filament-harness --glb Duck.glb --map-cam cam.json --out out.raw
//                                            → MapLibre-camera render, RGBA dump

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
#include <gltfio/ResourceLoader.h>
#include <gltfio/materials/uberarchive.h>

#include <math/mat4.h>
#include <math/vec3.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace filament;

constexpr uint32_t W = 800, H = 600;

namespace {

struct MapCam {
    double proj[16];
    double centerX;
    double centerY;
    double metersPerPixel;
};

// Minimal parser for the cam.json written by DebugCubeLayerHost:
// {"proj":[...16 doubles...],"centerX":..,"centerY":..,"metersPerPixel":..}
bool parseMapCam(const char* path, MapCam& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), {});

    auto readDoublesAfter = [&s](const char* key, double* dst, int count) -> bool {
        auto pos = s.find(key);
        if (pos == std::string::npos) return false;
        pos = s.find_first_of("0123456789-+.", pos + std::strlen(key));
        for (int i = 0; i < count; ++i) {
            if (pos == std::string::npos) return false;
            char* end = nullptr;
            dst[i] = std::strtod(s.c_str() + pos, &end);
            pos = s.find_first_of("0123456789-+.", end - s.c_str() + 1);
            if (i + 1 < count && pos == std::string::npos) return false;
        }
        return true;
    };

    return readDoublesAfter("\"proj\"", out.proj, 16) && readDoublesAfter("\"centerX\"", &out.centerX, 1) &&
           readDoublesAfter("\"centerY\"", &out.centerY, 1) &&
           readDoublesAfter("\"metersPerPixel\"", &out.metersPerPixel, 1);
}

void writePPM(const char* path, const uint8_t* rgba) {
    FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%u %u\n255\n", W, H);
    // readPixels returns rows bottom-up (GL convention); flip while writing
    for (int32_t row = H - 1; row >= 0; --row) {
        for (uint32_t col = 0; col < W; ++col) {
            std::fwrite(rgba + (row * W + col) * 4, 1, 3, f);
        }
    }
    std::fclose(f);
}

void writeRGBA(const char* path, const uint8_t* rgba) {
    FILE* f = std::fopen(path, "wb");
    for (int32_t row = H - 1; row >= 0; --row) {
        std::fwrite(rgba + row * W * 4, 1, W * 4, f);
    }
    std::fclose(f);
}

} // namespace

int main(int argc, char** argv) {
    const char* glbPath = nullptr;
    const char* mapCamPath = nullptr;
    const char* outPath = nullptr;
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--glb")) glbPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--map-cam")) mapCamPath = argv[i + 1];
        if (!std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
    }

    Engine* engine = Engine::create(backend::Backend::METAL);
    SwapChain* swapChain = engine->createSwapChain(W, H);
    Renderer* renderer = engine->createRenderer();
    Scene* scene = engine->createScene();
    View* view = engine->createView();

    utils::Entity camEntity = utils::EntityManager::get().create();
    Camera* camera = engine->createCamera(camEntity);

    view->setViewport({0, 0, W, H});
    view->setScene(scene);
    view->setCamera(camera);

    const bool transparent = mapCamPath != nullptr;
    renderer->setClearOptions({.clearColor = transparent ? math::double4{0.0, 0.0, 0.0, 0.0}
                                                         : math::double4{0.2, 0.4, 0.8, 1.0},
                               .clear = true});
    if (transparent) {
        view->setBlendMode(View::BlendMode::TRANSLUCENT);
    }

    gltfio::AssetLoader* loader = nullptr;
    gltfio::MaterialProvider* materials = nullptr;
    gltfio::FilamentAsset* asset = nullptr;

    if (glbPath) {
        materials = gltfio::createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        loader = gltfio::AssetLoader::create({engine, materials});

        std::ifstream in(glbPath, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", glbPath);
            return 1;
        }
        std::vector<uint8_t> glb((std::istreambuf_iterator<char>(in)), {});
        asset = loader->createAsset(glb.data(), static_cast<uint32_t>(glb.size()));
        if (!asset) {
            std::fprintf(stderr, "createAsset failed\n");
            return 1;
        }

        gltfio::ResourceConfiguration rc{};
        rc.engine = engine;
        rc.gltfPath = ".";
        rc.normalizeSkinningWeights = true;
        gltfio::ResourceLoader resourceLoader(rc);
        if (!resourceLoader.loadResources(asset)) {
            std::fprintf(stderr, "loadResources failed\n");
            return 1;
        }
        asset->releaseSourceData();

        scene->addEntities(asset->getEntities(), asset->getEntityCount());

        utils::Entity sun = utils::EntityManager::get().create();
        LightManager::Builder(LightManager::Type::DIRECTIONAL)
            .color({0.98f, 0.92f, 0.89f})
            .intensity(110000.0f)
            .direction(math::float3{0.5f, -1.0f, -0.8f})
            .castShadows(false)
            .build(*engine, sun);
        scene->addEntity(sun);

        const Aabb aabb = asset->getBoundingBox();
        const math::float3 center = aabb.center();
        const math::float3 extent = aabb.extent();

        if (mapCamPath) {
            MapCam mc{};
            if (!parseMapCam(mapCamPath, mc)) {
                std::fprintf(stderr, "cannot parse %s\n", mapCamPath);
                return 1;
            }

            // P' = P_map × translate(anchor): anchor rebase in double precision so
            // Filament's float entity transforms only ever see small values.
            math::mat4 P(mc.proj[0], mc.proj[1], mc.proj[2], mc.proj[3],
                         mc.proj[4], mc.proj[5], mc.proj[6], mc.proj[7],
                         mc.proj[8], mc.proj[9], mc.proj[10], mc.proj[11],
                         mc.proj[12], mc.proj[13], mc.proj[14], mc.proj[15]);
            const math::mat4 anchor = math::mat4::translation(math::double3{mc.centerX, mc.centerY, 0.0});
            camera->setCustomProjection(P * anchor, 0.1, 100000.0);

            // Model: glTF +Y-up meters → map frame (+z up, x/y world pixels, z meters).
            // sizeMeters tall, standing on the ground at the anchor.
            const double sizeMeters = 30.0;
            const double meshHeight = extent.y * 2.0; // Aabb extent is half-extent
            const double k = sizeMeters / meshHeight; // mesh units → meters
            const double sxy = k / mc.metersPerPixel; // meters → world pixels for x/y
            const double sz = k;                      // z stays meters

            auto& tcm = engine->getTransformManager();
            auto root = tcm.getInstance(asset->getRoot());
            const math::mat4f local = math::mat4f::scaling(math::float3{(float)sxy, (float)sxy, (float)sz}) *
                                      math::mat4f::rotation((float)M_PI_2, math::float3{1, 0, 0}) *
                                      math::mat4f::translation(
                                          math::float3{-center.x, -(center.y - extent.y), -center.z});
            tcm.setTransform(root, local);
        } else {
            camera->setProjection(45.0, double(W) / H, 0.1, 1000.0);
            camera->lookAt(math::double3{center.x + extent.x * 2, center.y + extent.y * 2, center.z + extent.z * 4},
                           math::double3{center.x, center.y, center.z},
                           math::double3{0, 1, 0});
        }
    }

    std::vector<uint8_t> pixels(W * H * 4);
    bool done = false;

    // Several frames: resource upload + stream work happen across frames.
    // After issuing readPixels, keep pumping frames — the completion callback is
    // delivered while the engine processes its command/callback queue.
    bool issued = false;
    for (int frame = 0; frame < 60 && !done; ++frame) {
        if (renderer->beginFrame(swapChain)) {
            renderer->render(view);
            if (frame >= 9 && !issued) {
                issued = true;
                backend::PixelBufferDescriptor pbd(
                    pixels.data(),
                    pixels.size(),
                    backend::PixelDataFormat::RGBA,
                    backend::PixelDataType::UBYTE,
                    [](void*, size_t, void* user) { *static_cast<bool*>(user) = true; },
                    &done);
                renderer->readPixels(0, 0, W, H, std::move(pbd));
            }
            renderer->endFrame();
        }
        engine->flushAndWait();
    }

    if (outPath) {
        writeRGBA(outPath, pixels.data());
        std::printf("done=%d wrote %s (raw RGBA %ux%u, top-down)\n", done, outPath, W, H);
    } else {
        const char* path = glbPath ? "/tmp/harness-duck.ppm" : "/tmp/harness-clear.ppm";
        writePPM(path, pixels.data());
        std::printf("done=%d wrote %s\n", done, path);
    }

    if (asset) {
        loader->destroyAsset(asset);
        gltfio::AssetLoader::destroy(&loader);
        materials->destroyMaterials();
    }
    Engine::destroy(&engine);
    return 0;
}
