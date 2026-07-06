#pragma once

#include <mbgl/style/layers/custom_drawable_layer.hpp>

namespace mbgl {
namespace model {

using CubeVertexVector = gfx::VertexVector<style::CustomDrawableLayerHost::Interface::GeometryVertex>;
using CubeIndexVector = gfx::IndexVector<gfx::Triangles>;

/// Unit cube for placeholder model rendering: x,y in [-0.5, 0.5], z in [0, 1]
/// (+z up, base on the ground plane), 4 vertices per face with per-face
/// texcoords addressing the 3x2 face-color texture.
void buildPlaceholderCube(CubeVertexVector& vertices, CubeIndexVector& indices);

/// 3x2 texture with one distinct color per cube face (nearest-filtered).
gfx::Texture2DPtr createFaceColorTexture(gfx::Context& context);

} // namespace model
} // namespace mbgl
