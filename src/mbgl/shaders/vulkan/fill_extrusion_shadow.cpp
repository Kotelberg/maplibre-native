#include <mbgl/shaders/vulkan/fill_extrusion_shadow.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this shader.
using FillExtrusionShadowShaderSource = ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Vulkan>;

const std::array<AttributeInfo, 5> FillExtrusionShadowShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Short4, idFillExtrusionNormalEdVertexAttribute},

    // Data driven
    AttributeInfo{2, gfx::AttributeDataType::Float4, idFillExtrusionColorVertexAttribute},
    AttributeInfo{3, gfx::AttributeDataType::Float, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{4, gfx::AttributeDataType::Float, idFillExtrusionHeightVertexAttribute},
};

const std::array<TextureInfo, 4> FillExtrusionShadowShaderSource::textures = {
    TextureInfo{0, idFillExtrusionShadowTexture0},
    TextureInfo{1, idFillExtrusionShadowTexture1},
    TextureInfo{2, idFillExtrusionShadowTexture2},
    TextureInfo{3, idFillExtrusionShadowTexture3},
};

} // namespace shaders
} // namespace mbgl
