#include <mbgl/shaders/mtl/fill_extrusion_shadow.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using FillExtrusionShadowShaderSource = ShaderSource<BuiltIn::FillExtrusionShadowShader, gfx::Backend::Type::Metal>;

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this shader;
// buffer indices are relative to this shader's own UBO count.
#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced path: receives shadows on the ROOF (sharedTriangles over footprint verts, which carry
// ed_discard not normal_ed). Drops normal_ed (roof normal is the constant up vector); color/base/height
// shift down to attribute slots 1/2/3.
const std::array<AttributeInfo, 4> FillExtrusionShadowShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, fillExtrusionShadowUBOCount + 0, idFillExtrusionPosVertexAttribute},
    AttributeInfo{
        1, gfx::AttributeDataType::Float4, fillExtrusionShadowUBOCount + 1, idFillExtrusionColorVertexAttribute},
    AttributeInfo{
        2, gfx::AttributeDataType::Float2, fillExtrusionShadowUBOCount + 1, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{
        3, gfx::AttributeDataType::Float2, fillExtrusionShadowUBOCount + 1, idFillExtrusionHeightVertexAttribute},
};
#else
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
#endif

// One texture per cascade (cascaded shadow maps); the receiver picks the tightest cascade.
const std::array<TextureInfo, 4> FillExtrusionShadowShaderSource::textures = {
    TextureInfo{0, idFillExtrusionShadowTexture0},
    TextureInfo{1, idFillExtrusionShadowTexture1},
    TextureInfo{2, idFillExtrusionShadowTexture2},
    TextureInfo{3, idFillExtrusionShadowTexture3},
};

} // namespace shaders
} // namespace mbgl
