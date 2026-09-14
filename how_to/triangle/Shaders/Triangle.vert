#version 450

layout(location = 0) out vec3 tint;

void main() {
    const vec2 positions[3] = vec2[](
        vec2( 0.0, -0.62),
        vec2( 0.62, 0.48),
        vec2(-0.62, 0.48)
    );
    const vec3 colors[3] = vec3[](
        vec3(1.0, 0.12, 0.08),
        vec3(0.55, 0.0, 0.03),
        vec3(0.12, 0.0, 0.015)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    tint = colors[gl_VertexIndex];
}
