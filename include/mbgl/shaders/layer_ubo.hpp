#pragma once

#include <mbgl/util/color.hpp>

#include <array>
#include <cstdint>

namespace mbgl {
namespace shaders {

enum class AttributeSource : int32_t {
    Constant,
    PerVertex,
    Computed,
};

enum class ExpressionFunction : int32_t {
    Constant,
    Linear,
    Exponential,
};

struct Expression {
    /* 0 */ ExpressionFunction function;
    /* 4 */
};
static_assert(sizeof(Expression) == 4);

struct Attribute {
    /* 0 */ AttributeSource source;
    /* 4 */ Expression expression;
    /* 8 */
};
static_assert(sizeof(Attribute) == 8);

//
// Global UBOs

struct alignas(16) GlobalPaintParamsUBO {
    /*  0 */ std::array<float, 2> pattern_atlas_texsize;
    /*  8 */ std::array<float, 2> units_to_pixels;
    /* 16 */ std::array<float, 2> world_size;
    /* 24 */ float camera_to_center_distance;
    /* 28 */ float symbol_fade_change;
    /* 32 */ float aspect_ratio;
    /* 36 */ float pixel_ratio;
    /* 40 */ float map_zoom;
    /* 44 */ float pad1;
    /* 48 */
};
static_assert(sizeof(GlobalPaintParamsUBO) == 3 * 16);

#if MLN_RENDER_BACKEND_VULKAN
struct alignas(16) GlobalPlatformParamsUBO {
    /*  0 */ alignas(16) std::array<float, 2> rotation0;
    /* 16 */ alignas(16) std::array<float, 2> rotation1;
    /* 32 */
};
static_assert(sizeof(GlobalPlatformParamsUBO) == 2 * 16);
#endif

enum {
    idGlobalPaintParamsUBO,
#if MLN_RENDER_BACKEND_METAL
    idGlobalUBOIndex,
#elif MLN_RENDER_BACKEND_VULKAN
    idGlobalPlatformParamsUBO,
#elif MLN_RENDER_BACKEND_WEBGPU
    idGlobalUBOIndex,
#endif
    globalUBOCount
};

#define MLN_UBO_CONSOLIDATION (MLN_RENDER_BACKEND_METAL || MLN_RENDER_BACKEND_VULKAN || MLN_RENDER_BACKEND_WEBGPU)
// Fork: Metal + GLES/Android use the NON-instancing fill-extrusion path; Vulkan stays
// INSTANCED (upstream's memory optimization — "Optimize fill extrusion memory by using
// instancing", #4256 — which matters most on the perf-first Vulkan/Android backend).
// NOTE (2026-06-16, verified on Metal): the instanced path DOES interpolate building
// height smoothly per frame (`unpack_mix_float(in_height, height_t)`; the OutlineInstance
// buffer stores only xy + edge, no baked height) — at the configured ramp midpoint,
// buildings render at half height. So the earlier "instanced snaps between heights"
// rationale was a misdiagnosis;
// the real (and only) reason Metal/GLES are non-instanced is that the SHIPPED cast-shadow
// caster/receiver consume per-vertex `normal_ed`, which the instanced bucket doesn't carry.
// Vulkan keeps instancing for perf; its cast shadows are built on the instanced geometry
// (reconstructing the wall normal in-shader from instance edges) rather than forcing it
// non-instanced — see renderer/shadows + vulkan/{shadow_depth,fill_extrusion_shadow}.
#define MLN_USE_FILL_EXTRUSION_INSTANCING (MLN_RENDER_BACKEND_METAL || MLN_RENDER_BACKEND_VULKAN)

// GL-only edge-indexed fill-extrusion instancing (Plan 1). Independent of
// MLN_USE_FILL_EXTRUSION_INSTANCING (which stays Metal/Vulkan). Default off during bring-up;
// flip to 1 to enable the OpenGL instanced path.
// See docs/superpowers/specs/2026-06-17-android-gl-fill-extrusion-instancing-design.md
#if !defined(MLN_GL_FE_INSTANCING)
#define MLN_GL_FE_INSTANCING 0
#endif

} // namespace shaders
} // namespace mbgl
