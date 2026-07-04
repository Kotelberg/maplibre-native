layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec4 a_normal_ed;

layout (std140) uniform ShadowDepthDrawableUBO {
    highp mat4 u_light_matrix;
    highp float u_base_t;
    highp float u_height_t;
    highp float u_base;
    highp float u_height;
};

// The [0,1] light-space depth metric the receiver compares against (the light_matrix bakes the
// Metal/Vulkan [0,1] z remap). Kept separate from gl_Position.z, which is remapped to GL's [-1,1]
// NDC below so the hardware depth test runs at full precision.
out highp float v_depth01;

#ifndef HAS_UNIFORM_u_base
layout (location = 2) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 3) in highp vec2 a_height;
#endif

void main() {
    // Match the visible FE / receiver vertex EXACTLY so the caster co-locates with the building you
    // see: same interpolation factor (NOT a hardcoded 0.0) in the data-driven branch.
    #ifndef HAS_UNIFORM_u_base
    highp float base = unpack_mix_vec2(a_base, u_base_t);
    #else
    highp float base = u_base;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = unpack_mix_vec2(a_height, u_height_t);
    #else
    highp float height = u_height;
    #endif

    base = max(0.0, base);
    height = max(0.0, height);

    // t (top/bottom flag) selects height vs base for z, matching the FE vertex.
    highp float t = mod(a_normal_ed.x, 2.0);
    highp vec4 clip = u_light_matrix * vec4(a_pos, t > 0.0 ? height : base, 1.0);
    // Pack the [0,1] light-space depth (matches the receiver's ndc.z metric).
    v_depth01 = clip.z / clip.w;
    // Remap z from the matrix's [0,1] (Metal/Vulkan NDC) to GL's [-1,1] NDC so the offscreen depth
    // test uses the FULL depth buffer. Without this the caster occupies only the upper half of the
    // range -> halved precision -> roof self-shadow acne (clean on Metal, broken on GL).
    clip.z = 2.0 * clip.z - clip.w;
    gl_Position = clip;
}
