#version 460
// glslang only honours #include when this is enabled first.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT vec3 hitColor;

void main()
{
    // A ray that hits nothing sees the same hemisphere the surfaces are lit
    // by, so the background and the ambient term cannot disagree.
    hitColor = HemisphereLight(normalize(gl_WorldRayDirectionEXT));
}
