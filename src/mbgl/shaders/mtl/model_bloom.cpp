#include <mbgl/shaders/mtl/model_bloom.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using ModelBloomShaderSource = ShaderSource<BuiltIn::ModelBloomShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> ModelBloomShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Float2, modelBloomUBOCount + 0, idModelBloomPosVertexAttribute},
};

const std::array<TextureInfo, 1> ModelBloomShaderSource::textures = {TextureInfo{0, idModelBloomImageTexture}};

} // namespace shaders
} // namespace mbgl
