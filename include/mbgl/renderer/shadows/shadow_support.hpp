#pragma once

// MLN_DRAWABLE_SHADOWS: 1 on backends that ship a full directional cast-shadow implementation
// (caster + receiver shaders + an offscreen render target with a depth attachment).
//
// The shared shadow C++ plumbing — the renderer-owned ShadowPass in RenderOrchestrator and the
// per-layer caster/receiver/ground wiring in RenderFillExtrusionLayer — is gated on this macro so
// backends WITHOUT shadow shaders stay byte-identical to stock MapLibre. The renderer plumbing
// itself is backend-agnostic (it uses only gfx:: abstractions); only the three shaders + the
// depth-capable offscreen target are backend-specific.
//
// Metal shipped first (iOS); OpenGL ES (Android) added 2026-06. Vulkan is NOT yet here: it stays on
// the INSTANCED fill-extrusion path for performance (see MLN_USE_FILL_EXTRUSION_INSTANCING), and the
// shipped caster/receiver consume per-vertex normal_ed which the instanced bucket doesn't carry. A
// Vulkan port ("Route A") that builds the caster/receiver on the instanced geometry (reconstructing
// the wall normal in-shader) is the planned path — flip Vulkan on here once those shaders land. The
// depth-capable vulkan/offscreen_texture target + the Vulkan bits in shadow_pass/shadow_tweakers are
// already in place for it. WebGPU flips on here once its shaders + depth target land.
#if MLN_RENDER_BACKEND_METAL || MLN_RENDER_BACKEND_OPENGL
#define MLN_DRAWABLE_SHADOWS 1
#else
#define MLN_DRAWABLE_SHADOWS 0
#endif
