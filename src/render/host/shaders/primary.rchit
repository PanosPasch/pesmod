#version 460
// glslang only honours #include when this is enabled first.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing_position_fetch : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT vec3 hitColor;
layout(location = 1) rayPayloadEXT   float shadowVisibility;

hitAttributeEXT vec2 attribs;

void main()
{
    // ── Normals ──────────────────────────────────────────────────────────
    // The game supplies none for its 24-byte pre-lit vertex layout, so a
    // geometric normal is derived from the triangle itself. Position fetch
    // hands us the vertices directly, which avoids binding every mesh's
    // vertex and index buffers into the hit shader just to read three
    // positions.
    //
    // This gives faceted shading. Smooth normals would need the game's own,
    // which exist only for the 32-byte lit layout — a later refinement, and
    // one that has to be fed through the scene stream first.
    const vec3 p0 = gl_HitTriangleVertexPositionsEXT[0];
    const vec3 p1 = gl_HitTriangleVertexPositionsEXT[1];
    const vec3 p2 = gl_HitTriangleVertexPositionsEXT[2];

    const vec3 objectNormal = normalize(cross(p1 - p0, p2 - p0));

    // Object-to-world for normals is the inverse transpose, but the game's
    // transforms are rigid plus uniform scale, so the 3x3 suffices once
    // renormalised.
    vec3 worldNormal = normalize(vec3(gl_ObjectToWorldEXT * vec4(objectNormal, 0.0)));

    // Orient against the viewer. The scene has plenty of single-sided
    // geometry drawn with culling disabled, so a back-facing hit is normal
    // rather than an error.
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    const vec3 hitPos = gl_WorldRayOriginEXT +
                        gl_WorldRayDirectionEXT * gl_HitTEXT;

    // ── Direct light, with a traced shadow ───────────────────────────────
    const vec3  L      = normalize(-scene.lightDirection.xyz);
    const float nDotL  = max(dot(worldNormal, L), 0.0);

    shadowVisibility = 0.0;
    if (nDotL > 0.0)
    {
        // Offset along the normal so the surface does not shadow itself.
        // The scene's units are roughly centimetres (a pitch is on the order
        // of 10,500 x 6,800 units), so the bias is scaled accordingly.
        const float bias = 0.5;

        traceRayEXT(topLevel,
                    gl_RayFlagsTerminateOnFirstHitEXT |
                    gl_RayFlagsSkipClosestHitShaderEXT |
                    gl_RayFlagsOpaqueEXT,
                    0xFF,
                    0,          // sbtRecordOffset
                    0,          // sbtRecordStride
                    1,          // missIndex: the shadow miss shader
                    hitPos + worldNormal * bias, 0.0,
                    L, max(scene.params.x, 1.0),
                    1);         // payload location
    }

    // ── Shading ──────────────────────────────────────────────────────────
    // Matches what vs_0007 does per vertex: a clamped N.L against the
    // directional light, plus a hemisphere term, plus ambient.
    const vec3 direct   = scene.lightColor.rgb * nDotL * shadowVisibility;
    const vec3 indirect = HemisphereLight(worldNormal) + scene.ambient.rgb;

    // Albedo is a placeholder until textures are bound into the hit shader.
    // Using the game's baked vertex colour here would double-light the
    // scene, since that colour already contains its own lighting — see
    // docs/RENDERER.md 4.3.
    const vec3 albedo = vec3(0.72);

    hitColor = albedo * (direct + indirect);
}
