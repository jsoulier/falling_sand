#version 450

layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, r32ui) uniform readonly uimage2D readImage;

const vec3 Colors[4] = vec3[](
    vec3(0.0f, 0.0f, 0.0f), /* empty */
    vec3(1.0f, 1.0f, 1.0f), /* stone */
    vec3(1.0f, 1.0f, 0.0f), /* sand */
    vec3(1.0f, 1.0f, 1.0f)  /* water */
);

void main()
{
    uint particle = imageLoad(readImage, ivec2(gl_FragCoord.xy)).x;
    outColor = vec4(Colors[particle], 1.0f);
}