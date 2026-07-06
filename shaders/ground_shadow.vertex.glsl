layout (location = 0) in vec2 a_pos;

layout (std140) uniform GroundShadowDrawableUBO {
    highp mat4 u_matrix;
    highp mat4 u_light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    highp int u_cascade_count;
    highp float u_drawable_pad0;
    highp float u_drawable_pad1;
    highp float u_drawable_pad2;
};

out highp vec4 v_shadow_pos[4];
out highp float v_view_w;
flat out int v_cascade_count;

void main() {
    highp vec4 worldLocal = vec4(a_pos, 0.0, 1.0);
    highp vec4 clip = u_matrix * worldLocal;
    gl_Position = clip;
    v_view_w = clip.w; // perspective view-distance for the near->far depth fade
    v_cascade_count = u_cascade_count;
    v_shadow_pos[0] = u_light_matrix[0] * worldLocal;
    v_shadow_pos[1] = u_light_matrix[u_cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = u_light_matrix[u_cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = u_light_matrix[u_cascade_count > 3 ? 3 : 0] * worldLocal;
}
