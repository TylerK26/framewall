#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vuv;
layout(location = 0) out vec4 color;
void main() { color = vec4(texture(tex, vuv).rgb, 1.0); }
