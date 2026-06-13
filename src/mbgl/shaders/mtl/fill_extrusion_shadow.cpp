#include <mbgl/shaders/mtl/fill_extrusion_shadow.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using FillExtrusionShadowShaderSource = ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Metal>;

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this shader;
// buffer indices are relative to this shader's own UBO count.
const std::array<AttributeInfo, 5> FillExtrusionShadowShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, fillExtrusionShadowUBOCount + 0, idFillExtrusionPosVertexAttribute},
    AttributeInfo{
        1, gfx::AttributeDataType::Short4, fillExtrusionShadowUBOCount + 0, idFillExtrusionNormalEdVertexAttribute},
    AttributeInfo{
        2, gfx::AttributeDataType::Float4, fillExtrusionShadowUBOCount + 1, idFillExtrusionColorVertexAttribute},
    AttributeInfo{
        3, gfx::AttributeDataType::Float2, fillExtrusionShadowUBOCount + 1, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{
        4, gfx::AttributeDataType::Float2, fillExtrusionShadowUBOCount + 1, idFillExtrusionHeightVertexAttribute},
};

const std::array<TextureInfo, 1> FillExtrusionShadowShaderSource::textures = {
    TextureInfo{0, idFillExtrusionShadowTexture}};

} // namespace shaders
} // namespace mbgl
