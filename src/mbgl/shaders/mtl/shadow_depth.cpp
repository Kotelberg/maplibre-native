#include <mbgl/shaders/mtl/shadow_depth.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using ShadowDepthShaderSource = ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Metal>;

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this caster.
#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced path: the caster draws the ROOF (sharedTriangles over footprint verts). Those verts carry
// ed_discard, not normal_ed, so the caster drops normal_ed and uses a constant t=1 (roof = top);
// base/height shift to attribute slots 1/2.
const std::array<AttributeInfo, 3> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 0, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionHeightVertexAttribute},
};
#else
const std::array<AttributeInfo, 4> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 0, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Short4, shadowDepthUBOCount + 0, idFillExtrusionNormalEdVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{3, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionHeightVertexAttribute},
};
#endif

#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced WALL caster: static unit-quad pos (per-vertex), per-edge OutlineInstance
// (pos + ed_discard), and data-driven base/height (per-instance). OutlinePos/EdDiscard
// are read in-shader via the raw `outline` buffer (buffer shadowDepthUBOCount+1), so
// they appear as instance attributes only to bind that buffer — they are NOT declared
// in the shader's VertexStage (attribute slots 1/2 are unused there).
using ShadowDepthInstancedShaderSource = ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::Metal>;

const std::array<AttributeInfo, 1> ShadowDepthInstancedShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 0, idFillExtrusionPosVertexAttribute},
};
// Keep color in the metadata even though the caster doesn't shade; it keeps the
// source interleaved paint buffer alive while base/height are copied into caster-only buffers.
const std::array<AttributeInfo, 5> ShadowDepthInstancedShaderSource::instanceAttributes = {
    AttributeInfo{1, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 1, idFillExtrusionOutlinePosAttribute},
    AttributeInfo{2, gfx::AttributeDataType::UShort2, shadowDepthUBOCount + 1, idFillExtrusionEdDiscardAttribute},

    // Data driven
    AttributeInfo{3, gfx::AttributeDataType::Float4, shadowDepthUBOCount + 2, idFillExtrusionColorVertexAttribute},
    AttributeInfo{4, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 3, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{5, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 4, idFillExtrusionHeightVertexAttribute},
};
#endif

} // namespace shaders
} // namespace mbgl
