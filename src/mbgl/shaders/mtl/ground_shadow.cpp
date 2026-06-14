#include <mbgl/shaders/mtl/ground_shadow.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using GroundShadowShaderSource = ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> GroundShadowShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, groundShadowUBOCount + 0, idGroundShadowPosVertexAttribute},
};

const std::array<TextureInfo, 1> GroundShadowShaderSource::textures = {TextureInfo{0, idGroundShadowTexture}};

} // namespace shaders
} // namespace mbgl
