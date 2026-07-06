precision highp float;

layout (std140) uniform CustomGeometryDrawableUBO {
    highp mat4 u_matrix;
    highp vec4 u_color;
};

in vec2 frag_uv;
uniform sampler2D u_image;

void main() {
    fragColor = texture(u_image, frag_uv) * u_color;
}
