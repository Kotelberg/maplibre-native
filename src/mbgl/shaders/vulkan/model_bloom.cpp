#include <mbgl/shaders/vulkan/model_bloom.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/model_bloom_ubo.hpp>

namespace mbgl {
namespace shaders {

using ModelBloomShaderSource = ShaderSource<BuiltIn::ModelBloomShader, gfx::Backend::Type::Vulkan>;

const std::array<AttributeInfo, 1> ModelBloomShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Float2, idModelBloomPosVertexAttribute},
};

const std::array<TextureInfo, 1> ModelBloomShaderSource::textures = {
    TextureInfo{0, idModelBloomImageTexture},
};

} // namespace shaders
} // namespace mbgl
