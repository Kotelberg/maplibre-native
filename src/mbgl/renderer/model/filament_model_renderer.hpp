#pragma once

#if MLN_WITH_FILAMENT_MODELS

#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mbgl {
namespace model {

struct ModelInstanceSpec {
    std::string modelId;
    // mercator world fraction (zoom-independent)
    double worldFractionX = 0;
    double worldFractionY = 0;
    double latitude = 0;
    float sizeMeters = 20.0f;
    float yawDegrees = 0.0f;
    float opacity = 1.0f;
};

/// Renders model-layer instances with Filament, headless, using the map's own
/// camera (M2 recipe: clipFlipY × proj × translate(anchor), all double; small
/// fp32 instance transforms relative to the camera-center anchor).
/// One renderer per RenderModelLayer (per-map sharing is M5 polish).
class FilamentModelRenderer {
public:
    FilamentModelRenderer();
    ~FilamentModelRenderer();

    FilamentModelRenderer(const FilamentModelRenderer&) = delete;
    FilamentModelRenderer& operator=(const FilamentModelRenderer&) = delete;

    /// id → local GLB file path. Assets load lazily on first use and stay cached.
    void setAssets(std::map<std::string, std::string> idToPath);

    /// Clip-space crop: render only the NDC rect [cx±hx, cy±hy] of the map
    /// view, mapped to the full output image (per-model depth billboards, M4).
    struct CropRect {
        double cx = 0, cy = 0, hx = 1, hy = 1;
    };

    /// Render instances. proj = the map's nearClippedProjMatrix;
    /// anchor/worldSize/zoom describe the current camera. Returns a
    /// premultiplied RGBA image (rows top-down), or nullptr on failure.
    std::shared_ptr<PremultipliedImage> render(const std::vector<ModelInstanceSpec>& instances,
                                               const mat4& proj,
                                               double anchorWorldX,
                                               double anchorWorldY,
                                               double worldSize,
                                               double zoom,
                                               uint32_t width,
                                               uint32_t height,
                                               const CropRect* crop = nullptr);

private:
    struct Backend;
    std::unique_ptr<Backend> backend;
    std::map<std::string, std::string> assets;
};

} // namespace model
} // namespace mbgl

#endif // MLN_WITH_FILAMENT_MODELS
