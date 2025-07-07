#version 450

#include "config.hpp"

layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, r32ui) uniform readonly uimage2D readImage;

const vec3 Colors[4] = vec3[](
    vec3(0.0f, 0.0f, 0.0f), /* empty */
    vec3(0.5f, 0.5f, 0.5f), /* stone */
    vec3(1.0f, 1.0f, 0.0f), /* sand */
    vec3(0.5f, 0.5f, 1.0f)  /* water */
);

const uint Bitshifts[4] = uint[](
    PARTICLE0,
    PARTICLE1,
    PARTICLE2,
    PARTICLE3
);

void main()
{
    uint particles = imageLoad(readImage, ivec2(gl_FragCoord.xy)).x;
    for (int i = 0; i < 4; i++)
    {
        uint particle  = (particles >> Bitshifts[i]) & 0xFF;
        if (particle == EMPTY)
        {
            continue;
        }
        outColor = vec4(Colors[particle], 1.0f);
        return;
    }
    outColor = vec4(Colors[EMPTY], 1.0f);
}