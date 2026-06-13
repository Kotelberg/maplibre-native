#include <mbgl/shaders/mtl/shadow_depth.hpp>
#include <mbgl/shaders/shader_defines.hpp>

namespace mbgl {
namespace shaders {

using ShadowDepthShaderSource = ShaderSource<BuiltIn::ShadowDepthShader, gfx::Backend::Type::Metal>;

// Reuses the fill-extrusion vertex-attribute ids so the FE bucket binders feed this caster.
const std::array<AttributeInfo, 4> ShadowDepthShaderSource::attributes = {
    AttributeInfo{0, gfx::AttributeDataType::Short2, shadowDepthUBOCount + 0, idFillExtrusionPosVertexAttribute},
    AttributeInfo{1, gfx::AttributeDataType::Short4, shadowDepthUBOCount + 0, idFillExtrusionNormalEdVertexAttribute},
    AttributeInfo{2, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionBaseVertexAttribute},
    AttributeInfo{3, gfx::AttributeDataType::Float2, shadowDepthUBOCount + 1, idFillExtrusionHeightVertexAttribute},
};

} // namespace shaders
} // namespace mbgl
