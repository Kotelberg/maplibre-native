#include <mbgl/shaders/vulkan/shadow_depth.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this caster.
using ShadowDepthShaderSource = ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Vulkan>;

const std::array<AttributeInfo, 4> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Short4, idFillExtrusionNormalEdVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{3, gfx::AttributeDataType::Float, idFillExtrusionHeightVertexAttribute},
};

const std::array<TextureInfo, 0> ShadowDepthShaderSource::textures = {};

} // namespace shaders
} // namespace mbgl
