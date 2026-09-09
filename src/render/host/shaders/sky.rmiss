#version 460
// glslang only honours #include when this is enabled first.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT vec3 hitColor;

void main()
{
    // The background is not a surface, so it does not get the surface rig.
    //
    // Running GameLighting on a ray direction was a category error: that
    // function adds a directional term, a ground term and ambient because
    // that is what the game does to a *lit vertex*, and applying it to a
    // direction produced a flat brown sky.
    //
    // What the sky is really made of in this game is a textured dome, which
    // the builder deliberately keeps out of the TLAS - it sits about 75 units
    // from the camera and would occlude the entire stadium (see
    // kInstanceNoDepthWrite). Sampling that dome by ray direction is the
    // right answer and is on the roadmap; until then this is a gradient
    // between the game's own sky and ground colours, which at least agrees
    // with what the surfaces are lit by.
    const vec3 dir = normalize(gl_WorldRayDirectionEXT);

    // c93 points along the hemisphere axis and is not unit length, so it is
    // normalised here - unlike in the shading path, where the game dots it
    // raw and the magnitude is part of the result.
    const float t = clamp(dot(dir, normalize(scene.hemisphereAxis.xyz)) * 0.5 + 0.5,
                          0.0, 1.0);

    hitColor = mix(scene.groundColor.rgb, scene.skyColor.rgb, t) *
               scene.lightingScale.rgb + scene.ambient.rgb;
}
