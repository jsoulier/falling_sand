/*
    static.frag
    by Spatial
    05 July 2013
*/

/* modified by jsoulier */

#ifndef RANDOM_GLSL
#define RANDOM_GLSL

uint hash(uint x)
{
    x += (x << 10u);
    x ^= (x >>  6u);
    x += (x <<  3u);
    x ^= (x >> 11u);
    x += (x << 15u);
    return x;
}

float floatConstruct(uint m)
{
    const uint ieeeMantissa = 0x007FFFFFu;
    const uint ieeeOne = 0x3F800000u;
    m &= ieeeMantissa;
    m |= ieeeOne;
    float f = uintBitsToFloat(m);
    return f - 1.0;
}

uint hash(uvec2 v)
{
    return hash(v.x ^ hash(v.y));
}

uint hash(uvec3 v)
{
    return hash(v.x ^ hash(v.y) ^ hash(v.z));
}

uint hash(uvec4 v)
{
    return hash(v.x ^ hash(v.y) ^ hash(v.z) ^ hash(v.w));
}

float random(float x)
{
    return floatConstruct(hash(floatBitsToUint(x)));
}

float random(vec2 v)
{
    return floatConstruct(hash(floatBitsToUint(v)));
}

float random(vec3 v)
{
    return floatConstruct(hash(floatBitsToUint(v)));
}

float random(vec4 v)
{
    return floatConstruct(hash(floatBitsToUint(v)));
}

float random(int x)
{
    return random(float(x));
}

float random(ivec2 v)
{
    return random(vec2(v));
}

float random(ivec3 v)
{
    return random(vec3(v));
}

float random(ivec4 v)
{
    return random(vec4(v));
}

#endif