#include <mbgl/shaders/mtl/ground_shadow.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using GroundShadowShaderSource = ShaderSource<BuiltIn::GroundShadowShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> GroundShadowShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, groundShadowUBOCount + 0, idGroundShadowPosVertexAttribute},
};

// One texture per cascade (cascaded shadow maps); the receiver picks the tightest cascade.
const std::array<TextureInfo, 4> GroundShadowShaderSource::textures = {
    TextureInfo{0, idGroundShadowTexture0},
    TextureInfo{1, idGroundShadowTexture1},
    TextureInfo{2, idGroundShadowTexture2},
    TextureInfo{3, idGroundShadowTexture3},
};

} // namespace shaders
} // namespace mbgl
