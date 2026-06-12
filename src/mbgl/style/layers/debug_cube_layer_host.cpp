#include <mbgl/style/layers/debug_cube_layer_host.hpp>

#include <mbgl/gfx/drawable.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/projection.hpp>

#include <array>
#include <cstring>
#include <memory>

namespace mbgl {
namespace style {

namespace {

using Vertex = CustomDrawableLayerHost::Interface::GeometryVertex;
using VertexVector = gfx::VertexVector<Vertex>;
using TriangleIndexVector = gfx::IndexVector<gfx::Triangles>;

// 6 face colors laid out in a 3x2 texture (opaque, so premultiplied values
// equal straight values).
constexpr std::array<std::array<uint8_t, 4>, 6> kFaceColors{{
    {230, 57, 70, 255},   // +x east  red
    {29, 53, 87, 255},    // -x west  navy
    {42, 157, 143, 255},  // +y north teal
    {233, 196, 106, 255}, // -y south sand
    {244, 162, 97, 255},  // +z top   orange
    {38, 70, 83, 255},    // -z base  slate
}};

// Texcoord center of face cell `i` in the 3x2 texture.
std::array<float, 2> faceUV(size_t i) {
    const float u = (static_cast<float>(i % 3) + 0.5f) / 3.0f;
    const float v = (static_cast<float>(i / 3) + 0.5f) / 2.0f;
    return {u, v};
}

void appendFace(VertexVector& vertices,
                TriangleIndexVector& indices,
                const std::array<std::array<float, 3>, 4>& corners,
                size_t faceIndex) {
    const auto base = static_cast<uint16_t>(vertices.elements());
    const auto uv = faceUV(faceIndex);
    for (const auto& p : corners) {
        vertices.emplace_back(Vertex{.position = p, .texcoords = uv});
    }
    indices.emplace_back(base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2));
    indices.emplace_back(base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3));
}

gfx::Texture2DPtr createFaceColorTexture(CustomDrawableLayerHost::Interface& interface) {
    auto image = std::make_shared<PremultipliedImage>(Size(3, 2));
    constexpr size_t pixelSize = 4;
    for (size_t i = 0; i < kFaceColors.size(); ++i) {
        const size_t row = i / 3;
        const size_t col = i % 3;
        std::memcpy(image->data.get() + row * image->stride() + col * pixelSize, kFaceColors[i].data(), pixelSize);
    }
    auto texture = interface.context.createTexture2D();
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});
    texture->setImage(std::move(image));
    return texture;
}

} // namespace

DebugCubeLayerHost::DebugCubeLayerHost(LatLng location_, double sizeMeters_)
    : location(location_),
      sizeMeters(sizeMeters_) {}

void DebugCubeLayerHost::initialize() {}

void DebugCubeLayerHost::deinitialize() {}

void DebugCubeLayerHost::update(Interface& interface) {
    if (interface.getDrawableCount() > 0) {
        return;
    }

    const auto sharedVertices = std::make_shared<VertexVector>();
    const auto sharedIndices = std::make_shared<TriangleIndexVector>();

    // Model space: x,y in [-0.5, 0.5], z in [0, 1] (+z up, base on ground).
    constexpr float lo = -0.5f;
    constexpr float hi = 0.5f;
    constexpr float zb = 0.0f;
    constexpr float zt = 1.0f;

    appendFace(*sharedVertices, *sharedIndices, {{{hi, lo, zb}, {hi, hi, zb}, {hi, hi, zt}, {hi, lo, zt}}}, 0); // +x
    appendFace(*sharedVertices, *sharedIndices, {{{lo, hi, zb}, {lo, lo, zb}, {lo, lo, zt}, {lo, hi, zt}}}, 1); // -x
    appendFace(*sharedVertices, *sharedIndices, {{{hi, hi, zb}, {lo, hi, zb}, {lo, hi, zt}, {hi, hi, zt}}}, 2); // +y
    appendFace(*sharedVertices, *sharedIndices, {{{lo, lo, zb}, {hi, lo, zb}, {hi, lo, zt}, {lo, lo, zt}}}, 3); // -y
    appendFace(*sharedVertices, *sharedIndices, {{{lo, lo, zt}, {hi, lo, zt}, {hi, hi, zt}, {lo, hi, zt}}}, 4); // +z
    appendFace(*sharedVertices, *sharedIndices, {{{lo, hi, zb}, {hi, hi, zb}, {hi, lo, zb}, {lo, lo, zb}}}, 5); // -z

    Interface::GeometryOptions options;
    options.texture = createFaceColorTexture(interface);

    interface.setGeometryTweakerCallback(
        [loc = location, size = sizeMeters](
            gfx::Drawable&, const PaintParameters& params, Interface::GeometryOptions& currentOptions) {
            LatLng unwrapped = loc.wrapped();
            unwrapped.unwrapForShortestPath(params.state.getLatLng(LatLng::Wrapped));
            const Point<double> center = Projection::project(unwrapped, params.state.getScale());

            const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(loc.latitude(),
                                                                                  params.state.getZoom());
            const double s = size / metersPerPixel;

            mat4 m = matrix::identity4();
            matrix::translate(m, m, center.x, center.y, 0.0);
            // The map projection expects x/y in world pixels but z in METERS:
            // Camera::getWorldToCamera post-multiplies the z column by
            // pixelsPerMeter (src/mbgl/util/camera.cpp).
            matrix::scale(m, m, s, s, size);
            matrix::multiply(currentOptions.matrix, params.transformParams.nearClippedProjMatrix, m);
        });

    interface.setGeometryOptions(options);
    interface.addGeometry(sharedVertices, sharedIndices, /*is3D=*/true);

    interface.finish();
}

} // namespace style
} // namespace mbgl
