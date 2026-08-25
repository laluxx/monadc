#version 450

layout(location = 0) in vec2 canvas;
layout(location = 0) out vec4 color;

layout(push_constant) uniform Frame {
    float time;
    float aspect;
} frame;

float segment(vec2 point, vec2 from, vec2 to) {
    vec2 line = to - from;
    float along = clamp(dot(point - from, line) / dot(line, line), 0.0, 1.0);
    return length(point - from - line * along);
}

float star(vec2 point) {
    const float turn = 6.28318530718;
    vec2 points[5];

    for (int index = 0; index < 5; ++index) {
        float angle = turn * float(index) / 5.0 - turn * 0.25;
        points[index] = vec2(cos(angle), sin(angle)) * 0.52;
    }

    float distance = 10.0;
    for (int index = 0; index < 5; ++index)
        distance = min(distance, segment(point, points[index], points[(index + 2) % 5]));

    return distance;
}

float bloodNoise(vec2 point) {
    return fract(sin(dot(point, vec2(41.13, 289.71))) * 45758.5453);
}

void main() {
    vec2 point = vec2(canvas.x * frame.aspect, canvas.y);
    float pulse = 0.5 + 0.5 * sin(frame.time * 1.7);
    float radius = length(point);

    float smoke = bloodNoise(floor(point * 90.0 + frame.time));
    smoke *= smoothstep(1.35, 0.05, radius) * 0.025;

    float ring = abs(radius - 0.68);
    float sigil = min(star(point), ring);
    float ink = smoothstep(0.026 + pulse * 0.006, 0.004, sigil);
    float halo = exp(-18.0 * sigil) * (0.15 + pulse * 0.12);

    vec3 blackBlood = vec3(0.008, 0.001, 0.003);
    vec3 arterial = vec3(0.72, 0.008, 0.018);
    vec3 hotBlood = vec3(1.0, 0.055, 0.025);
    vec3 scene = blackBlood + vec3(smoke, 0.0, 0.0);
    scene += arterial * halo;
    scene = mix(scene, mix(arterial, hotBlood, pulse * 0.45), ink);
    scene *= 1.0 - smoothstep(0.45, 1.6, radius) * 0.82;

    color = vec4(scene, 1.0);
}
