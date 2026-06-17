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
// Metal shipped first (iOS); OpenGL ES (Android) added 2026-06; Vulkan landed via "Route A": the
// caster/receiver are built on the INSTANCED fill-extrusion geometry (the instanced bucket carries
// ed_discard, not per-vertex normal_ed, so the roof caster/receiver use the FE_INSTANCING constants
// t=1 / normal=(0,0,1) and a dedicated instanced WALL caster (ShadowDepthInstancedShader) reconstructs
// the wall quads from the per-edge OutlineInstance SSBO). The depth-capable vulkan/offscreen_texture
// target + the Vulkan bits in shadow_pass/shadow_tweakers were already in place. WebGPU flips on here
// once its shaders + depth target land.
#if MLN_RENDER_BACKEND_METAL || MLN_RENDER_BACKEND_OPENGL || MLN_RENDER_BACKEND_VULKAN
#define MLN_DRAWABLE_SHADOWS 1
#else
#define MLN_DRAWABLE_SHADOWS 0
#endif
