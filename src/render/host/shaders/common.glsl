// common.glsl — shared bindings and helpers for the ray tracing pipeline.
//
// ── Matrix convention ────────────────────────────────────────────────────
//
// The game is Direct3D 8 and uses the row-vector convention: clip = v * M,
// with M stored row-major. GLSL stores a mat4 column-major and evaluates
// M * v. Those two cancel out exactly: uploading our row-major row-vector
// matrix verbatim and writing `M * v` in GLSL computes `v * M` in the
// original convention. So no transpose happens anywhere on this path — see
// scene_math.h, which documents the same hazard on the CPU side.

#extension GL_EXT_ray_tracing : require

struct SceneUniforms
{
    // Inverse of the view-projection the game's shaders actually used,
    // recovered by AccelBuilder::ResolveViewProjection rather than taken
    // from SetTransform.
    mat4 invViewProj;

    // The game's own lighting rig, read out of vertex shader constants
    // c95/c94/c93/c92/c91/c68 — not invented. See docs/RENDERER.md 4.3.
    vec4 lightDirection;    // c95, unit length
    vec4 lightColor;        // c94
    vec4 hemisphereAxis;    // c93
    vec4 skyColor;          // c92
    vec4 groundColor;       // c91
    vec4 ambient;           // c68

    vec4 params;            // x = shadow ray max distance, y = exposure
};

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevel;
layout(binding = 1, set = 0, rgba8) uniform image2D outputImage;
layout(binding = 2, set = 0) uniform SceneBlock { SceneUniforms scene; };

// Unprojects a point on the near or far plane back into world space.
// D3D depth runs 0 at the near plane to 1 at the far plane, which is what
// the game's projection produces and therefore what its inverse expects.
vec3 UnprojectToWorld(vec2 ndc, float depth)
{
    vec4 h = scene.invViewProj * vec4(ndc, depth, 1.0);
    return h.xyz / h.w;
}

// Hemisphere ambient: sky above, ground below, blended by how far the normal
// leans along the axis. This is the same term vs_0007 computes per vertex
// with dp3 against c93 and a 0.5/0.5 remap.
vec3 HemisphereLight(vec3 normal)
{
    float t = dot(normal, -normalize(scene.hemisphereAxis.xyz)) * 0.5 + 0.5;
    return mix(scene.groundColor.rgb, scene.skyColor.rgb, t);
}
