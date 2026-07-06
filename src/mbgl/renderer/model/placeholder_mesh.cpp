#include <mbgl/renderer/model/placeholder_mesh.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/util/image.hpp>

#include <array>
#include <cstring>

namespace mbgl {
namespace model {

namespace {

using Vertex = style::CustomDrawableLayerHost::Interface::GeometryVertex;

// 6 face colors laid out in a 3x2 texture (opaque, so premultiplied values
// equal straight values).
constexpr std::array<std::array<uint8_t, 4>, 6> kFaceColors{{
    {230, 57, 70, 255},   // +x east  red
    {29, 53, 87, 255},    // -x west  navy
    {42, 157, 143, 255},  // +y (projected south) teal
    {233, 196, 106, 255}, // -y (projected north) sand
    {244, 162, 97, 255},  // +z top   orange
    {38, 70, 83, 255},    // -z base  slate
}};

// Texcoord center of face cell `i` in the 3x2 texture.
std::array<float, 2> faceUV(size_t i) {
    const float u = (static_cast<float>(i % 3) + 0.5f) / 3.0f;
    const float v = (static_cast<float>(i / 3) + 0.5f) / 2.0f;
    return {u, v};
}

void appendFace(CubeVertexVector& vertices,
                CubeIndexVector& indices,
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

} // namespace

void buildPlaceholderCube(CubeVertexVector& vertices, CubeIndexVector& indices) {
    constexpr float lo = -0.5f;
    constexpr float hi = 0.5f;
    constexpr float zb = 0.0f;
    constexpr float zt = 1.0f;

    appendFace(vertices, indices, {{{hi, lo, zb}, {hi, hi, zb}, {hi, hi, zt}, {hi, lo, zt}}}, 0); // +x
    appendFace(vertices, indices, {{{lo, hi, zb}, {lo, lo, zb}, {lo, lo, zt}, {lo, hi, zt}}}, 1); // -x
    appendFace(vertices, indices, {{{hi, hi, zb}, {lo, hi, zb}, {lo, hi, zt}, {hi, hi, zt}}}, 2); // +y
    appendFace(vertices, indices, {{{lo, lo, zb}, {hi, lo, zb}, {hi, lo, zt}, {lo, lo, zt}}}, 3); // -y
    appendFace(vertices, indices, {{{lo, lo, zt}, {hi, lo, zt}, {hi, hi, zt}, {lo, hi, zt}}}, 4); // +z
    appendFace(vertices, indices, {{{lo, hi, zb}, {hi, hi, zb}, {hi, lo, zb}, {lo, lo, zb}}}, 5); // -z
}

gfx::Texture2DPtr createFaceColorTexture(gfx::Context& context) {
    auto image = std::make_shared<PremultipliedImage>(Size(3, 2));
    constexpr size_t pixelSize = 4;
    for (size_t i = 0; i < kFaceColors.size(); ++i) {
        const size_t row = i / 3;
        const size_t col = i % 3;
        std::memcpy(image->data.get() + row * image->stride() + col * pixelSize, kFaceColors[i].data(), pixelSize);
    }
    auto texture = context.createTexture2D();
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});
    texture->setImage(std::move(image));
    return texture;
}

} // namespace model
} // namespace mbgl
