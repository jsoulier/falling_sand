#version 450
    
/* NOTE: don't know what it's called but definitely not a blur */

layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, rgba32f) uniform readonly image2D readImage;

const ivec2 Offsets[5] = ivec2[](
    ivec2(0, 0),
    ivec2(0, -1),
    ivec2(0, 1),
    ivec2(-1, 0),
    ivec2(1, 0)
);

void main()
{
    vec4 color = vec4(0.0f);
    for (int i = 0; i < 5; i++)
    {
        ivec2 position = ivec2(gl_FragCoord.xy) + Offsets[i];
        vec4 otherColor = imageLoad(readImage, position);
        if (length(otherColor) > length(color))
        {
            color = otherColor;
        }
    }
    outColor = color;
}