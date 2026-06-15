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

// One texture per cascade (cascaded shadow maps); the receiver picks the tightest cascade.
const std::array<TextureInfo, 4> FillExtrusionShadowShaderSource::textures = {
    TextureInfo{0, idFillExtrusionShadowTexture0},
    TextureInfo{1, idFillExtrusionShadowTexture1},
    TextureInfo{2, idFillExtrusionShadowTexture2},
    TextureInfo{3, idFillExtrusionShadowTexture3},
};

} // namespace shaders
} // namespace mbgl
