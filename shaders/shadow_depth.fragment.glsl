precision highp float;

in highp float v_depth01;

vec4 packDepth(float depth) {
    const float maxPackable = 1.0 - 1.0 / 16581375.0;
    depth = clamp(depth, 0.0, maxPackable);
    const vec4 bitSh = vec4(1.0, 255.0, 65025.0, 16581375.0);
    const vec4 mask  = vec4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    vec4 enc = fract(bitSh * depth);
    enc -= enc.yzww * mask;
    return enc;
}

void main() {
    // Pack the [0,1] light-space depth — the exact metric the receiver compares against.
    fragColor = packDepth(v_depth01);
}
