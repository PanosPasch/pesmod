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

    // The game modulates the texture by the vertex colour, so a blended
    // surface is covered by the product of both alphas. Resolved before the
    // albedo branches so every path multiplies the same value in.
    float vertexAlpha = 1.0;

    const uint debugView = uint(scene.debug.x + 0.5);
    if (debugView == kDebugInstance)
    {
        payload.colour = IndexColour(uint(gl_InstanceCustomIndexEXT));
        payload.alpha  = 1.0;
        payload.dist   = gl_HitTEXT;
        return;
    }
    if (debugView == kDebugFlags)
    {
        payload.colour = vec3(
            ((rec.flags & kRecordBlended)     != 0u) ? 1.0 : 0.0,
            ((rec.flags & kRecordSpriteBatch) != 0u) ? 1.0 : 0.0,
            ((rec.flags & kRecordUnlit)       != 0u) ? 1.0 : 0.0);
        payload.alpha = 1.0;
        payload.dist  = gl_HitTEXT;
        return;
    }

    // attribs holds the last two barycentrics; the first is what remains.
    const vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

    // Resolved once, because the albedo and the normal both need it and
    // they must not be allowed to disagree about which triangle was hit.
    const bool haveVertices = (rec.vertexAddress != 0ul) &&
                              ((rec.flags & kRecordSpriteBatch) == 0u);
    const uvec3 tri = haveVertices ? HitTriangle(rec, gl_PrimitiveID)
                                   : uvec3(0u);

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
    else if (haveVertices && rec.uvOffset != kNoVertexAttribute)
    {
        FloatData verts = FloatData(rec.vertexAddress);

        const vec2 uv = bary.x * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.x)
                      + bary.y * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.y)
                      + bary.z * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.z);

        // LOD 0 rather than an implicit level: a ray tracing stage has no
        // derivatives. It is sharp and will alias in the distance; choosing
        // a level from ray differentials is the proper fix and comes later.
        const vec4 sampled = SampleInstance(rec, uv);
        albedo *= sampled.rgb;
        if ((rec.flags & kRecordVertexAlpha) != 0u)
            vertexAlpha = HitVertexAlpha(rec, tri, bary);
        if ((rec.flags & kRecordBlended) != 0u)
            surfaceAlpha = sampled.a * rec.baseColor.a * vertexAlpha;
    }

    if (debugView == kDebugAlbedo)
    {
        payload.colour = albedo;
        payload.alpha  = 1.0;
        payload.dist   = gl_HitTEXT;
        return;
    }

    // ── Unlit surfaces stop here ─────────────────────────────────────────
    // The sky is a pre-lit texture on a box around the camera. It has no
    // meaningful normal, wants no shadow ray, and must not be run through the
    // stadium's lighting rig - and returning before the recursive trace is
    // also what keeps the sky lookup from adding a level of recursion.
    if ((rec.flags & kRecordUnlit) != 0u)
    {
        payload.colour = albedo;
        payload.alpha  = surfaceAlpha;
        payload.dist   = gl_HitTEXT;
        return;
    }

    // ── Normals ──────────────────────────────────────────────────────────
    //
    // The game's lit layouts carry a per-vertex normal; its 24-byte pre-lit
    // layout carries a baked colour instead and has none. So a surface uses
    // its own normals when it has them, and a geometric one when it does
    // not - which is 96% of the geometries in a recorded stream by count,
    // but the ones that matter most, the players, are all in the 4% that do.
    //
    // A skinned mesh's normals are rewritten by the palette in the same pass
    // that moves its positions, so this reads a posed normal, not a rest one.
    //
    // Position fetch hands the triangle's vertices over directly, which is
    // what makes the geometric fallback free: no vertex buffer needs binding
    // to read three positions.
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
    vec3 objectNormal = (area2 > 1e-20)
                      ? crossed / area2
                      : -normalize(gl_WorldRayDirectionEXT);

    if (haveVertices && rec.normalOffset != kNoVertexAttribute)
    {
        FloatData verts = FloatData(rec.vertexAddress);
        const vec3 interpolated =
              bary.x * VertexNormal(verts, rec.vertexStride, rec.normalOffset, tri.x)
            + bary.y * VertexNormal(verts, rec.vertexStride, rec.normalOffset, tri.y)
            + bary.z * VertexNormal(verts, rec.vertexStride, rec.normalOffset, tri.z);

        // Opposed influences in the palette can cancel a skinned normal to
        // nothing, and normalizing that is a NaN that survives to the end as
        // a black patch. The geometric normal is already computed and is the
        // right thing to keep when that happens.
        const float len = length(interpolated);
        if (len > 1e-6) objectNormal = interpolated / len;
    }

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

    if (debugView == kDebugNormal)
    {
        payload.colour = worldNormal * 0.5 + 0.5;
        payload.alpha  = 1.0;
        payload.dist   = gl_HitTEXT;
        return;
    }

    // ── Sky occlusion ────────────────────────────────────────────────────
    //
    // The game's c92 term adds sky light to every surface as though nothing
    // were ever in the way, because a rasteriser cannot ask what is. Asking
    // is the first thing on this path the original renderer could not have
    // done at all.
    //
    // The rays are short on purpose. A bounded reach makes this contact
    // occlusion rather than a global term, which is both far less noisy at a
    // handful of samples and closer to what is actually missing from the
    // game's rig: the darkening where a player meets the grass, or where the
    // stand steps meet their risers.
    //
    // This runs before the shadow ray, not after, because both use the same
    // payload location - so whichever goes last is the one whose answer is
    // still there when the shading runs.
    float skyVisibility = 1.0;
    const int aoSamples = int(scene.params.z);
    if (aoSamples > 0)
    {
        const float reach = max(scene.params.w, 1.0);

        // Relative to how far the ray travelled, not an absolute distance.
        // Float precision scales with distance, so a fixed offset that
        // clears the surface up close is lost in the noise far away - and a
        // ray starting inside its own surface reports occlusion at random,
        // which is grain rather than shading.
        //
        // It has to be relative for a second reason: the self-test's scene
        // is a unit cube, where the half-unit the shadow ray uses would put
        // every occlusion ray outside the geometry entirely and quietly
        // report that nothing occludes anything.
        const float aoBias = max(gl_HitTEXT * 2.0e-4, 1.0e-4);
        // Seeded on where the hit is in the world, not on which pixel it
        // landed in.
        //
        // A screen-space seed makes the sample pattern belong to the screen:
        // hold the scene still and orbit the camera - which is exactly what
        // the game's replay mode does - and the occlusion crawls across
        // every surface, which reads as the shadows themselves moving. A
        // world-space seed pins the pattern to the surface, so it sits still
        // under a moving camera and reads as grain in the material.
        //
        // Quantised to a fraction of the ray reach so that neighbouring
        // pixels on one surface still differ, while the same point keeps its
        // directions from frame to frame.
        const vec3 cell = floor(hitPos / max(reach * 0.02, 1.0e-3));
        uint rng = HashCombine(HashCombine(uint(int(cell.x)) * 73856093u,
                                           uint(int(cell.y)) * 19349663u),
                               uint(int(cell.z)) * 83492791u);

        // Stratified, not independent: the samples are spread one per cell
        // of a regular grid over the unit square and jittered inside it,
        // which is what stops eight independent draws from clumping and
        // reporting a visibility that no direction actually saw. One random
        // rotation per pixel keeps neighbouring pixels from sharing the
        // pattern and turning the variance into a visible tiling.
        const float rot = RandomFloat(rng);
        int unoccluded = 0;
        for (int i = 0; i < aoSamples; ++i)
        {
            const float u1 = (float(i) + RandomFloat(rng)) / float(aoSamples);
            const float u2 = fract(rot + RadicalInverse(uint(i)));
            const vec3 d = CosineHemisphere(worldNormal, u1, u2);

            // The same question as a shadow ray, so the same flags, the same
            // miss shader and the same payload; only the direction and the
            // reach differ.
            shadowVisibility = 0.0;
            traceRayEXT(topLevel,
                        gl_RayFlagsTerminateOnFirstHitEXT |
                        gl_RayFlagsSkipClosestHitShaderEXT,
                        kMaskShadow,
                        0, 0, 1,
                        hitPos + worldNormal * aoBias, 0.0,
                        d, reach,
                        1);
            if (shadowVisibility > 0.5) ++unoccluded;
        }
        skyVisibility = float(unoccluded) / float(aoSamples);
    }

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
                    kMaskShadow,   // overlays write no depth, so cast none
                    0,          // sbtRecordOffset
                    0,          // sbtRecordStride
                    1,          // missIndex: the shadow miss shader
                    hitPos + worldNormal * bias, 0.0,
                    L, max(scene.params.x, 1.0),
                    1);         // payload location
    }

    if (debugView == kDebugSkyVis || debugView == kDebugShadow)
    {
        const float v = (debugView == kDebugSkyVis) ? skyVisibility
                                                    : shadowVisibility;
        payload.colour = vec3(v);
        payload.alpha  = 1.0;
        payload.dist   = gl_HitTEXT;
        return;
    }

    // ── Shading ──────────────────────────────────────────────────────────
    // The whole rig, transcribed from vs_0007 in common.glsl rather than
    // reassembled here.
    const vec3 lit = GameLighting(worldNormal, shadowVisibility, skyVisibility);

    // Note what is deliberately *not* used: the 24-byte layout's vertex
    // colour. It already contains the game's own baked lighting, so folding
    // it in here would light the scene twice — see docs/RENDERER.md 4.3.

    payload.colour = albedo * lit;
    payload.alpha  = surfaceAlpha;
    payload.dist   = gl_HitTEXT;
}
