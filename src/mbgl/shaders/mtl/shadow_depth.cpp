#include <mbgl/shaders/mtl/shadow_depth.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using ShadowDepthShaderSource = ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 3> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 0, idShadowDepthPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Float, shadowDepthUBOCount + 0, idShadowDepthBaseVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float, shadowDepthUBOCount + 0, idShadowDepthHeightVertexAttribute},
};

} // namespace shaders
} // namespace mbgl
