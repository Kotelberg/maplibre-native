layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec4 a_normal_ed;

layout (std140) uniform FillExtrusionShadowDrawableUBO {
    highp mat4 u_matrix;
    highp mat4 u_light_matrix[4]; // one per concentric cascade (near->far); first cascade_count valid
    highp float u_base_t;
    highp float u_height_t;
    highp float u_color_t;
    highp int u_cascade_count;
    highp float drawable_pad0;
    highp float drawable_pad1;
    highp float drawable_pad2;
    highp float drawable_pad3;
};

layout (std140) uniform FillExtrusionShadowPropsUBO {
    highp vec4 u_color;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_base; // xyz = light dir, w = base (uniform path)
    highp float u_height;
    highp float u_light_intensity;
    highp float u_vertical_gradient;
    highp float u_opacity;
    highp float u_shadow_intensity;
    highp float u_shadow_texel_size;
    highp float u_shadow_bias;
    highp float u_shadow_slope_bias;
};

out highp vec4 v_color;
out highp vec4 v_shadow_pos[4];
out highp float v_slope;
out highp float v_wallness;
flat out int v_cascade_count;

#ifndef HAS_UNIFORM_u_color
layout (location = 4) in highp vec4 a_color;
#endif
#ifndef HAS_UNIFORM_u_base
layout (location = 2) in highp vec2 a_base;
#endif
#ifndef HAS_UNIFORM_u_height
layout (location = 3) in highp vec2 a_height;
#endif

void main() {
    #ifndef HAS_UNIFORM_u_base
    highp float base = max(unpack_mix_vec2(a_base, u_base_t), 0.0);
    #else
    highp float base = u_light_position_base.w;
    #endif
    #ifndef HAS_UNIFORM_u_height
    highp float height = max(unpack_mix_vec2(a_height, u_height_t), 0.0);
    #else
    highp float height = u_height;
    #endif

    highp float t = mod(a_normal_ed.x, 2.0);
    highp vec3 normal = a_normal_ed.xyz / 16384.0;
    highp float z = t > 0.0 ? height : base;
    highp vec4 worldLocal = vec4(a_pos, z, 1.0);
    gl_Position = u_matrix * worldLocal;

    #ifndef HAS_UNIFORM_u_color
    highp vec4 color = unpack_mix_color(a_color, u_color_t);
    #else
    highp vec4 color = u_color;
    #endif

    highp float luminance = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;
    v_color = vec4(0.0, 0.0, 0.0, 1.0);
    color += min(vec4(0.03, 0.03, 0.03, 1.0), vec4(1.0));

    highp float directionalFraction = clamp(dot(normal, u_light_position_base.xyz), 0.0, 1.0);
    highp float minDirectional = 1.0 - u_light_intensity;
    highp float maxDirectional = max(1.0 - luminance + u_light_intensity, 1.0);
    highp float directional = mix(minDirectional, maxDirectional, directionalFraction);

    if (normal.y != 0.0) {
        highp float fMin = mix(0.7, 0.98, 1.0 - u_light_intensity);
        highp float factor = clamp((t + base) * pow(height / 150.0, 0.5), fMin, 1.0);
        directional *= (1.0 - u_vertical_gradient) + (u_vertical_gradient * factor);
    }

    highp vec3 light_color = u_light_color_pad.rgb;
    highp vec3 minLight = mix(vec3(0.0), vec3(0.3), 1.0 - light_color);
    v_color += vec4(clamp(color.rgb * directional * light_color, minLight, vec3(1.0)), 0.0);
    v_color *= u_opacity;

    // (1 - n·L): 0 on sun-facing faces, ->1 on faces turned away; scales the depth bias so a building
    // never shadows its OWN away-faces.
    v_slope = 1.0 - directionalFraction;
    // 1.0 on vertical walls (normal.z~0), 0.0 on roofs (normal.z~+-1).
    v_wallness = 1.0 - abs(normal.z);
    v_cascade_count = u_cascade_count;

    // Project into every cascade's light clip (near->far). Unused slots replicate cascade 0.
    v_shadow_pos[0] = u_light_matrix[0] * worldLocal;
    v_shadow_pos[1] = u_light_matrix[u_cascade_count > 1 ? 1 : 0] * worldLocal;
    v_shadow_pos[2] = u_light_matrix[u_cascade_count > 2 ? 2 : 0] * worldLocal;
    v_shadow_pos[3] = u_light_matrix[u_cascade_count > 3 ? 3 : 0] * worldLocal;
}
