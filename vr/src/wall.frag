// Decoder NV12 (sampled through a YCbCr conversion: BT.709 limited range ->
// R'G'B') -> the sRGB wall image. The hardware re-encodes to sRGB on write,
// so decode the sRGB curve first: the wall gets the desktop's exact pixels.
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { vec2 scale; } pc;  // wall pixel -> texture coordinate
layout(location = 0) out vec4 color;
vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
void main() {
    vec3 rgb = clamp(texture(tex, gl_FragCoord.xy * pc.scale).rgb, 0.0, 1.0);
    color = vec4(to_linear(rgb), 1.0);
}
