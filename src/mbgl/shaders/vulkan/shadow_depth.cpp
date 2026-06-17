#include <mbgl/shaders/vulkan/shadow_depth.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this caster.
using ShadowDepthShaderSource = ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Vulkan>;

#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced path: the caster draws the ROOF (sharedTriangles over footprint verts; ed_discard, not
// normal_ed). Drop normal_ed and shift base/height up to slots 1/2.
const std::array<AttributeInfo, 3> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Float, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float, idFillExtrusionHeightVertexAttribute},
};
#else
const std::array<AttributeInfo, 4> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Short4, idFillExtrusionNormalEdVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{3, gfx::AttributeDataType::Float, idFillExtrusionHeightVertexAttribute},
};
#endif

const std::array<TextureInfo, 0> ShadowDepthShaderSource::textures = {};

#if MLN_USE_FILL_EXTRUSION_INSTANCING
// Instanced WALL caster: mirrors the visible FillExtrusionInstancedShader binding layout — static unit
// quad pos (per-vertex), per-edge OutlineInstance pos+ed_discard routed into the SSBO via the
// idFillExtrusionInstanced tag, and data-driven color/base/height (per-instance). color is kept in the
// metadata to anchor the interleaved instance paint buffer even though the caster doesn't shade.
using ShadowDepthInstancedShaderSource = ShaderSource<BuiltIn::ShadowDepthInstancedShader, gfx::Backend::Type::Vulkan>;

const std::array<AttributeInfo, 1> ShadowDepthInstancedShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, idFillExtrusionPosVertexAttribute},
};
const std::array<AttributeInfo, 5> ShadowDepthInstancedShaderSource::instanceAttributes = {
    AttributeInfo{1, gfx::AttributeDataType::Short2, idFillExtrusionOutlinePosAttribute, idFillExtrusionInstanced},
    AttributeInfo{2, gfx::AttributeDataType::UShort2, idFillExtrusionEdDiscardAttribute, idFillExtrusionInstanced},

    // Data driven
    AttributeInfo{3, gfx::AttributeDataType::Float4, idFillExtrusionColorVertexAttribute},
    AttributeInfo{4, gfx::AttributeDataType::Float, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{5, gfx::AttributeDataType::Float, idFillExtrusionHeightVertexAttribute},
};
#endif

} // namespace shaders
} // namespace mbgl
