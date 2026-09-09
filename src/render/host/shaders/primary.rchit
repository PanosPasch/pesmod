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
    // ── Albedo ───────────────────────────────────────────────────────────
    //
    // ORDER MATTERS. This block must stay ahead of the shadow ray below.
    //
    // Sampling the texture *after* the recursive traceRayEXT produced a black
    // surface on this driver — not a wrong colour, but every shading term in
    // the shader reading as zero, including ones the sample does not touch.
    // Each piece measured correct in isolation and only the combination
    // failed, and moving the sample above the trace fixes it exactly:
    // the textured surface then measures 0.184/0.599/0.851 against an
    // expected texture 0.2/0.6/0.9 times lighting 0.92/1.00/0.95.
    //
    // Sampling before tracing is the natural order anyway, so this is not a
    // workaround so much as a reason not to "tidy" it back. The self-test's
    // texture check is what will notice if anyone does.
    //
    // The instance says where its vertices and indices live and which
    // texture slot it uses; nothing about the surface was known before the
    // ray landed on it.
    const InstanceRecord rec = instances[gl_InstanceCustomIndexEXT];

    vec3 albedo = rec.baseColor.rgb;
    if (rec.vertexAddress != 0ul && rec.uvOffset != kNoVertexAttribute)
    {
        FloatData verts = FloatData(rec.vertexAddress);

        // The three corner indices of the triangle that was hit.
        uvec3 tri;
        if (rec.indexStride != 0u)
        {
            WordData idx = WordData(rec.indexAddress);
            const uint base = gl_PrimitiveID * 3u;
            tri = uvec3(IndexAt(idx, rec.indexStride, base),
                        IndexAt(idx, rec.indexStride, base + 1u),
                        IndexAt(idx, rec.indexStride, base + 2u));
        }
        else
        {
            const uint base = gl_PrimitiveID * 3u;
            tri = uvec3(base, base + 1u, base + 2u);
        }

        // attribs holds the last two barycentrics; the first is what remains.
        const vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

        const vec2 uv = bary.x * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.x)
                      + bary.y * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.y)
                      + bary.z * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.z);

        // textureLod, not texture: a ray tracing stage has no derivatives,
        // so there is no implicit mip level to compute. LOD 0 is sharp and
        // will alias in the distance; picking a level from ray differentials
        // is the proper fix and comes later.
        //
        // The slot varies between neighbouring rays, so the index is not
        // uniform across the subgroup and has to say so.
        albedo *= textureLod(textures[nonuniformEXT(rec.textureSlot)],
                             uv, 0.0).rgb;
    }

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

        // Note the absence of gl_RayFlagsOpaqueEXT: the shadow ray runs the
        // any-hit shader too, so the pitch's transparent overlays do not cast
        // shadows on the grass they lie on. That costs traversal time and
        // buys correctness where it is most visible.
        traceRayEXT(topLevel,
                    gl_RayFlagsTerminateOnFirstHitEXT |
                    gl_RayFlagsSkipClosestHitShaderEXT,
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

    // Note what is deliberately *not* used: the 24-byte layout's vertex
    // colour. It already contains the game's own baked lighting, so folding
    // it in here would light the scene twice — see docs/RENDERER.md 4.3.

    hitColor = albedo * (direct + indirect);
}
