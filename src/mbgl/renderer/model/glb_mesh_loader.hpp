#pragma once

#include <mbgl/style/layers/custom_drawable_layer.hpp>
#include <mbgl/util/image.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mbgl {
namespace model {

/// A GLB baked into map-renderable static geometry ("Approach D"): node
/// transforms applied, glTF +Y-up converted to the map's +Z-up, base centered
/// at the origin on z=0, uniformly scaled so the model height is exactly 1
/// (the per-instance tweaker scales by sizeMeters, like the M1 cube).
/// Rendered unlit with baseColor textures — rock-solid under camera motion
/// and per-pixel depth-correct, at the cost of PBR lighting (the Filament
/// path remains for that).
struct BakedModel {
    struct Part {
        std::shared_ptr<gfx::VertexVector<style::CustomDrawableLayerHost::Interface::GeometryVertex>> vertices;
        std::shared_ptr<gfx::IndexVector<gfx::Triangles>> indices;
        std::shared_ptr<PremultipliedImage> texture; // null → use color
        Color color = Color::white();
    };
    std::vector<Part> parts;
    bool valid = false;
};

/// Parse + bake a GLB file. Primitives are split so every part stays within
/// uint16 index range. Returns an invalid model on any parse failure.
BakedModel loadGlbMesh(const std::string& path);

} // namespace model
} // namespace mbgl
