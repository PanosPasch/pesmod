#version 460
// glslang only honours #include when this is enabled first.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing_position_fetch : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;
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

    // Opaque surfaces replace what is behind them; blended ones composite,
    // and their alpha is the texture's times the draw's tint. An opaque
    // surface reports 1 so the ray generation stops there.
    float surfaceAlpha = 1.0;

    vec3 albedo = rec.baseColor.rgb;
    if ((rec.flags & kRecordSpriteBatch) != 0u)
    {
        // The merged sprite batch: its material lives per triangle, keyed by
        // gl_PrimitiveID, because the merge is what threw away which draw
        // each triangle came from. Everything else about shading it - the
        // geometric normal, the shadow ray, the lighting rig - is the same
        // as for any other surface.
        const SpriteTriangle spr = spriteTriangles[gl_PrimitiveID];
        const vec4 sampled = SampleSprite(spr, SpriteUv(spr, attribs));

        albedo = spr.baseColor.rgb * sampled.rgb;
        if ((spr.flags & kRecordBlended) != 0u)
            surfaceAlpha = sampled.a * spr.baseColor.a;
    }
    else if (rec.vertexAddress != 0ul && rec.uvOffset != kNoVertexAttribute)
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

        // LOD 0 rather than an implicit level: a ray tracing stage has no
        // derivatives. It is sharp and will alias in the distance; choosing
        // a level from ray differentials is the proper fix and comes later.
        const vec4 sampled = SampleInstance(rec, uv);
        albedo *= sampled.rgb;
        if ((rec.flags & kRecordBlended) != 0u)
            surfaceAlpha = sampled.a * rec.baseColor.a;
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

    // A zero-area triangle makes cross() zero and normalize() NaN, and NaN
    // survives every later multiply to be clamped to black at the end - a
    // solid dark patch on an otherwise correct surface, with no error
    // anywhere. Skinned meshes collapse triangles routinely when several
    // influences pull a vertex onto its neighbour, so this is not a rare
    // case; it accounted for the dark quads on players' chests.
    const vec3  crossed = cross(p1 - p0, p2 - p0);
    const float area2   = length(crossed);
    const vec3  objectNormal = (area2 > 1e-20)
                             ? crossed / area2
                             : -normalize(gl_WorldRayDirectionEXT);

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
    // c95 points toward the light and is used unnegated, exactly as the
    // game's `dp3 r11.x, v1, c95` does. Only its direction is wanted here -
    // the shadow ray needs a unit vector - while GameLighting below uses the
    // raw register, magnitude and all.
    const vec3  L      = normalize(scene.lightDirection.xyz);
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
    // The whole rig, transcribed from vs_0007 in common.glsl rather than
    // reassembled here.
    const vec3 lit = GameLighting(worldNormal, shadowVisibility);

    // Note what is deliberately *not* used: the 24-byte layout's vertex
    // colour. It already contains the game's own baked lighting, so folding
    // it in here would light the scene twice — see docs/RENDERER.md 4.3.

    payload.colour = albedo * lit;
    payload.alpha  = surfaceAlpha;
    payload.dist   = gl_HitTEXT;
}
