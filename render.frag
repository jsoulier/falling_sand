#version 450

#include "config.hpp"

layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, r32ui) uniform readonly uimage2D readImage;

void main()
{
    uint particle = imageLoad(readImage, ivec2(gl_FragCoord.xy)).x;
    float red = float((particle >> 0) & 0xFFu) / 255.0f;
    float green = float((particle >> 8) & 0xFFu) / 255.0f;
    float blue = float((particle >> 16) & 0xFFu) / 255.0f;
    outColor = vec4(red, green, blue, 1.0f);
}