layout (location = 0) in vec2 a_pos;            // static unit quad: x = endpoint sel, y = base/top sel
layout (location = 1) in vec2 a_pos0;           // instance: edge start (tile coords)
layout (location = 2) in vec2 a_pos1;           // instance: edge end
layout (location = 3) in vec3 a_normal0;        // instance: smoothed wall normal at pos0 * 2^14
layout (location = 4) in vec3 a_normal1;        // instance: smoothed wall normal at pos1 * 2^14
layout (location = 5) in highp float a_edgedistance; // instance: edge distance (pattern wrap; unused here)
out vec4 v_color;

layout (std140) uniform FillExtrusionDrawableUBO {
    highp mat4 u_matrix;
    highp vec2 u_pixel_coord_upper;
    highp vec2 u_pixel_coord_lower;
    highp float u_height_factor;
    highp float u_tile_ratio;
    // Interpolations
    highp float u_base_t;
    highp float u_height_t;
    highp float u_color_t;
    highp float u_pattern_from_t;
    highp float u_pattern_to_t;
    lowp float drawable_pad1;
};

layout (std140) uniform FillExtrusionTilePropsUBO {
    highp vec4 u_pattern_from;
    highp vec4 u_pattern_to;
    highp vec2 u_texsize;
    lowp float tileprops_pad1;
    lowp float tileprops_pad2;
};

layout (std140) uniform FillExtrusionPropsUBO {
    highp vec4 u_color;
    highp vec3 u_lightcolor;
    lowp float props_pad1;
    highp vec3 u_lightpos;
    highp float u_base;
    highp float u_height;
    highp float u_lightintensity;
    highp float u_vertical_gradient;
    highp float u_opacity;
    highp float u_fade;
    highp float u_from_scale;
    highp float u_to_scale;
    lowp float props_pad2;
};

#pragma mapbox: define highp float base
#pragma mapbox: define highp float height
#pragma mapbox: define highp vec4 color

void main() {
    #pragma mapbox: initialize highp float base
    #pragma mapbox: initialize highp float height
    #pragma mapbox: initialize highp vec4 color

    base = max(0.0, base);
    height = max(0.0, height);

    // Reconstruct the wall from this edge's two endpoints + the static unit quad:
    //   a_pos.x selects the endpoint (0 = start/pos0, 1 = end/pos1)
    //   a_pos.y selects base (0) vs top (1)
    // GLES 3.0 cannot fetch outline[gl_InstanceID + 1], so both endpoints + their
    // smoothed normals arrive as per-instance attributes (edge-indexed model).
    bool atStart = (a_pos.x < 0.5);
    vec2 footprint = atStart ? a_pos0 : a_pos1;
    float t = a_pos.y;                       // 0 = base, 1 = height

    gl_Position = u_matrix * vec4(footprint, t > 0.5 ? height : base, 1.0);

    // Smoothed wall normal at the selected endpoint (packed * 2^14; matches the
    // non-instanced FillExtrusionShader's normal / 16384.0).
    vec3 normal = atStart ? a_normal0 : a_normal1;

    // Relative luminance (how dark/bright is the surface color?)
    float colorvalue = color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722;

    v_color = vec4(0.0, 0.0, 0.0, 1.0);

    // Add slight ambient lighting so no extrusions are totally black
    vec4 ambientlight = vec4(0.03, 0.03, 0.03, 1.0);
    color += ambientlight;

    // Calculate cos(theta), where theta is the angle between surface normal and diffuse light ray
    float directional = clamp(dot(normal / 16384.0, u_lightpos), 0.0, 1.0);

    // Adjust directional so the range of values for highlight/shading is narrower
    // with lower light intensity and with lighter/brighter surface colors
    directional = mix((1.0 - u_lightintensity), max((1.0 - colorvalue + u_lightintensity), 1.0), directional);

    // Add gradient along z axis of side surfaces
    if (normal.y != 0.0) {
        directional *= (
            (1.0 - u_vertical_gradient) +
            (u_vertical_gradient * clamp((t + base) * pow(height / 150.0, 0.5), mix(0.7, 0.98, 1.0 - u_lightintensity), 1.0)));
    }

    // Assign final color based on surface + ambient light color, diffuse light directional, and light color
    v_color.r += clamp(color.r * directional * u_lightcolor.r, mix(0.0, 0.3, 1.0 - u_lightcolor.r), 1.0);
    v_color.g += clamp(color.g * directional * u_lightcolor.g, mix(0.0, 0.3, 1.0 - u_lightcolor.g), 1.0);
    v_color.b += clamp(color.b * directional * u_lightcolor.b, mix(0.0, 0.3, 1.0 - u_lightcolor.b), 1.0);
    v_color *= u_opacity;
}
