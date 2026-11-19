#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location = 0) out vec2 vuv;
void main() { vuv = uv; gl_Position = pc.mvp * vec4(pos, 1.0); }
