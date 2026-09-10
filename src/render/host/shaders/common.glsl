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
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

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
    vec4 lightingScale;     // c69

    // x = shadow ray max distance, y = exposure,
    // z = sky occlusion rays per hit (0 disables), w = their reach
    vec4 params;

    // x = the builder's decal bias per draw-order step, as a fraction of
    // the distance to the camera; y = how far into one such step the peel
    // resumes. Both come from the host so there is exactly one definition.
    vec4 decal;

    // x selects a debug view; see kDebug* below. Zero is the real image.
    vec4 debug;
};

// What a hit shader needs to shade a surface it did not know it would hit.
// A rasteriser binds a texture and a vertex layout per draw; a ray tracer
// cannot, so each instance publishes where its data is. Mirrors
// AccelBuilder::InstanceRecord, which static_asserts this layout.
struct InstanceRecord
{
    uint64_t vertexAddress;
    uint64_t indexAddress;
    uint     vertexStride;
    uint     uvOffset;      // bytes to the float2 UV, or 0xFFFFFFFF
    uint     indexStride;   // 2 or 4; 0 means non-indexed
    uint     textureSlot;
    vec4     baseColor;
    uint     samplerIndex;   // addressing belongs to the draw, not the image
    uint     flags;          // kRecordBlended
    uint     normalOffset;   // bytes to the float3 normal, or kNoVertexAttribute
    uint     colorOffset;    // bytes to the D3DCOLOR diffuse, or kNoVertexAttribute

    // The affine texture-coordinate transform the game's vertex shader
    // applied, or the identity. Two vec4s rather than six floats so std430
    // and C++ agree without a padding argument.
    //
    //     .xy of uvTransform0 scales u, .zw scales v, uvTransform1.xy offsets
    //
    // The game windows an atlas with it - one advert out of a sheet holding
    // every advert, one expression out of a face sheet - and sampling the
    // raw UV instead shows the whole sheet at once.
    vec4     uvTransform0;   // (a.x, a.y, b.x, b.y)
    vec4     uvTransform1;   // (offset.u, offset.v, unused, unused)
};

// uv, put through the draw's own texture transform.
vec2 TransformUv(vec4 t0, vec4 t1, vec2 uv)
{
    return vec2(uv.x * t0.x + uv.y * t0.z + t1.x,
                uv.x * t0.y + uv.y * t0.w + t1.y);
}

// ── Ray masks ────────────────────────────────────────────────────────────
//
// "Depth writes off" is the game saying a draw must not occlude anything.
// A mask says exactly that: an overlay is visible to the eye and invisible
// to a shadow ray. The sky is visible to neither, and is looked up on its
// own when a primary ray reaches the miss shader. Mirrors
// AccelBuilder::RayMask.
const uint kMaskSolid        = 0x01u;
const uint kMaskNonOccluding = 0x02u;
const uint kMaskSky          = 0x04u;
const uint kMaskPrimary      = kMaskSolid | kMaskNonOccluding;
const uint kMaskShadow       = kMaskSolid;

const uint kRecordBlended    = 1u;

// This record covers the merged sprite batch. Its triangles came from many
// different draws, so the record itself carries no usable material; the
// per-triangle table below does.
const uint kRecordSpriteBatch = 2u;

// Shade with the texture alone - no normal, no shadow ray, no lighting rig.
// The sky is a pre-lit texture, so running the stadium's light over it would
// light the thing that is the light.
const uint kRecordUnlit       = 4u;

// Coverage comes from the vertex colour's alpha as well as the texture's.
// Set by the builder only where the texture has no alpha of its own.
const uint kRecordVertexAlpha = 8u;

// ── The primary ray payload ──────────────────────────────────────────────
//
// The game composites its blended surfaces in draw order, so taking the
// nearest hit and shading it opaquely is wrong wherever anything is drawn
// over anything else - pitch markings, shirt numbers, projected shadows.
//
// So the hit shader reports what it found rather than a final colour, and the
// ray generation peels layers front to back, accumulating
// `colour * alpha * transmittance` and attenuating the transmittance as it
// goes. Accumulating front to back with a running transmittance is exactly
// equivalent to compositing back to front, and needs no sorting.
//
// `dist` is negative when nothing was hit, which is how the miss shader says
// "this is the background, stop here".
struct HitPayload
{
    vec3  colour;
    float alpha;
    float dist;
};

// ── Debug views ─────────────────────────────────────────────────────────
//
// Shading is a product of terms, and a wrong pixel says which pixel but not
// which term. Each of these writes one term out on its own, straight to the
// image with no exposure and no gamma, so a pixel can be read off and
// compared with what it was supposed to be.
const uint kDebugOff       = 0u;
const uint kDebugInstance  = 1u;   // a colour per TLAS instance
const uint kDebugAlbedo    = 2u;   // the texture sample and tint alone
const uint kDebugNormal    = 3u;   // the shading normal, encoded
const uint kDebugSkyVis    = 4u;   // traced sky occlusion
const uint kDebugShadow    = 5u;   // the directional shadow term
const uint kDebugFlags     = 6u;   // blended / sprite batch / unlit
const uint kDebugLayers    = 7u;   // how much of the peel a pixel used
const uint kDebugLeftover  = 8u;   // transmittance the peel had to drop

// Distinct neighbouring colours from consecutive indices: three irrational
// strides, so adjacent instances never land on adjacent colours.
vec3 IndexColour(uint i)
{
    return vec3(fract(float(i) * 0.6180339887 + 0.15),
                fract(float(i) * 0.4142135624 + 0.45),
                fract(float(i) * 0.2360679775 + 0.75));
}

// How many blended surfaces a single ray will composite before giving up.
// The pitch is seven coplanar layers, so this has to be comfortably more
// than that; beyond it the remaining transmittance is simply dropped.
const int kMaxLayers = 12;

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevel;
layout(binding = 1, set = 0, rgba8) uniform image2D outputImage;
layout(binding = 2, set = 0) uniform SceneBlock { SceneUniforms scene; };

// Every texture the scene could hit, reachable at once. Slot 0 is a 1x1
// white texture, so an untextured surface takes the same path with no branch.
//
// Images and samplers are separate so a texture can be sampled wrapped by
// one draw and clamped by another without holding two copies of it: the
// pitch grass tiles, while a projected shadow reads one blob out of a
// mostly-empty atlas and must clamp.
layout(binding = 3, set = 0) uniform texture2D textures[];
layout(binding = 5, set = 0) uniform sampler   samplers[];

// Pairs the instance's texture with the sampler its draw asked for. The slot
// varies between neighbouring rays, so it is not uniform across the subgroup
// and has to say so.
vec4 SampleInstance(InstanceRecord rec, vec2 uv)
{
    return textureLod(sampler2D(textures[nonuniformEXT(rec.textureSlot)],
                                samplers[nonuniformEXT(rec.samplerIndex)]),
                      uv, 0.0);
}

layout(binding = 4, set = 0, std430) readonly buffer InstanceBlock
{
    InstanceRecord instances[];
};

// ── The merged sprite batch's materials ──────────────────────────────────
//
// A sprite draw is two triangles, and there are around a hundred of them in
// a frame: advertising hoardings, stand panels, projected shadows. Each
// getting its own acceleration structure costs far more than tracing it, so
// the builder bakes them into world space and merges them into one - which
// throws away the thing a rasteriser gets for free, namely which draw a
// triangle came from. Every one of those draws is textured, so throwing it
// away is what left the batch rendering flat white.
//
// The batch is non-indexed, three vertices per triangle, so gl_PrimitiveID
// indexes this table directly. Mirrors AccelBuilder::SpriteTriangle, which
// static_asserts the layout.
struct SpriteTriangle
{
    vec2 uv0;
    vec2 uv1;
    vec2 uv2;
    uint textureSlot;
    uint samplerIndex;
    vec4 baseColor;
    uint flags;             // kRecordBlended
    uint _pad0, _pad1, _pad2;
};

layout(binding = 6, set = 0, std430) readonly buffer SpriteBlock
{
    SpriteTriangle spriteTriangles[];
};

// The sprite batch's UV at a hit, interpolated the same way HitUv does it.
vec2 SpriteUv(SpriteTriangle spr, vec2 bary2)
{
    const vec3 bary = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    return bary.x * spr.uv0 + bary.y * spr.uv1 + bary.z * spr.uv2;
}

vec4 SampleSprite(SpriteTriangle spr, vec2 uv)
{
    return textureLod(sampler2D(textures[nonuniformEXT(spr.textureSlot)],
                                samplers[nonuniformEXT(spr.samplerIndex)]),
                      uv, 0.0);
}

// Matches SceneIPC::kNoVertexAttribute: the offset a geometry reports for an
// attribute it does not have. Declared here because every vertex helper
// below tests against it.
const uint kNoVertexAttribute = 0xFFFFFFFFu;

// Vertex and index data is read through device addresses rather than bound
// buffers, because there is no "current mesh" at hit time — the record says
// where to look.
layout(buffer_reference, std430, buffer_reference_align = 4)
    readonly buffer FloatData { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4)
    readonly buffer WordData  { uint v[]; };

// 16-bit indices read out of 32-bit words. Doing it this way avoids requiring
// shader 16-bit storage for what is two lines of arithmetic; buffers are at
// least 4-byte aligned, so the word index is always in range.
uint IndexAt(WordData buf, uint stride, uint i)
{
    if (stride == 4u) return buf.v[i];
    const uint word = buf.v[i >> 1u];
    return ((i & 1u) == 0u) ? (word & 0xFFFFu) : (word >> 16u);
}

vec2 VertexUv(FloatData verts, uint stride, uint uvOffset, uint index)
{
    // Both offsets are multiples of 4 in every layout the game uses, so the
    // float index is exact.
    const uint base = (index * stride + uvOffset) >> 2u;
    return vec2(verts.v[base], verts.v[base + 1u]);
}

vec3 VertexNormal(FloatData verts, uint stride, uint normalOffset, uint index)
{
    const uint base = (index * stride + normalOffset) >> 2u;
    return vec3(verts.v[base], verts.v[base + 1u], verts.v[base + 2u]);
}

// The alpha of a vertex's D3DCOLOR diffuse.
//
// D3DCOLOR is 0xAARRGGBB, and every colorOffset the game uses is a multiple
// of four, so the whole thing is one word and the top byte is the alpha.
//
// Only the alpha is read. The game modulates texture by vertex colour, so a
// blended surface's coverage is the product of the two; its RGB on this
// layout is the game's own baked lighting, which this renderer replaces
// rather than reuses.
float VertexAlpha(WordData verts, uint stride, uint colorOffset, uint index)
{
    const uint word = verts.v[(index * stride + colorOffset) >> 2u];
    return float(word >> 24u) * (1.0 / 255.0);
}

// The three corner indices of the triangle that was hit. Non-indexed
// geometry numbers its vertices straight through.
uvec3 HitTriangle(InstanceRecord rec, uint primitiveID)
{
    const uint base = primitiveID * 3u;
    if (rec.indexStride == 0u) return uvec3(base, base + 1u, base + 2u);

    WordData idx = WordData(rec.indexAddress);
    return uvec3(IndexAt(idx, rec.indexStride, base),
                 IndexAt(idx, rec.indexStride, base + 1u),
                 IndexAt(idx, rec.indexStride, base + 2u));
}

// The interpolated vertex alpha at a hit, or 1 where there is none. Shared
// by the closest-hit and any-hit shaders for the same reason HitUv is: the
// alpha test and the shading must not disagree about how covered a surface
// is.
float HitVertexAlpha(InstanceRecord rec, uvec3 tri, vec3 bary)
{
    if (rec.vertexAddress == 0ul || rec.colorOffset == kNoVertexAttribute)
        return 1.0;

    WordData verts = WordData(rec.vertexAddress);
    return bary.x * VertexAlpha(verts, rec.vertexStride, rec.colorOffset, tri.x)
         + bary.y * VertexAlpha(verts, rec.vertexStride, rec.colorOffset, tri.y)
         + bary.z * VertexAlpha(verts, rec.vertexStride, rec.colorOffset, tri.z);
}

// The interpolated UV at a triangle hit. Shared by the closest-hit and
// any-hit shaders so the alpha test and the shading sample cannot disagree
// about where on the texture the ray landed.
//
// `bary2` is the hit attribute: the last two barycentrics, the first being
// whatever remains.
vec2 HitUv(InstanceRecord rec, uint primitiveID, vec2 bary2)
{
    FloatData verts = FloatData(rec.vertexAddress);

    uvec3 tri;
    const uint base = primitiveID * 3u;
    if (rec.indexStride != 0u)
    {
        WordData idx = WordData(rec.indexAddress);
        tri = uvec3(IndexAt(idx, rec.indexStride, base),
                    IndexAt(idx, rec.indexStride, base + 1u),
                    IndexAt(idx, rec.indexStride, base + 2u));
    }
    else
    {
        tri = uvec3(base, base + 1u, base + 2u);
    }

    const vec3 bary = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    const vec2 raw = bary.x * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.x)
                   + bary.y * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.y)
                   + bary.z * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.z);

    // Through the draw's transform, so an alpha-tested shadow ray tests the
    // same texels the eye sees. A scrolling hoarding whose shadow was cut
    // from a different part of the sheet would be worse than no transform.
    return TransformUv(rec.uvTransform0, rec.uvTransform1, raw);
}

// Below this a texel is treated as absent rather than composited. It is
// deliberately near zero: partial alpha is composited by the ray generation
// rather than tested away, and the only thing worth rejecting outright is a
// texel that would contribute nothing while consuming one of the peeling
// layers. Of the game's seven coplanar pitch layers, two are entirely below
// this.
const float kAlphaEpsilon = 1.0 / 255.0;

// Unprojects a point on the near or far plane back into world space.
// D3D depth runs 0 at the near plane to 1 at the far plane, which is what
// the game's projection produces and therefore what its inverse expects.
vec3 UnprojectToWorld(vec2 ndc, float depth)
{
    vec4 h = scene.invViewProj * vec4(ndc, depth, 1.0);
    return h.xyz / h.w;
}

// The game's lighting, transcribed from vs_0007 rather than approximated:
//
//     dp3  r11.x, v1, c95        ; N . light, c95 used as-is
//     max  r11.x, r11.x, c57.x   ; clamped at zero
//     mul  r10,   r11.x, c94     ; directional colour
//     dp3  r9.x,  v1, c93
//     mad  r9.x,  r9.x, 0.5, 0.5 ; hemisphere blend
//     mad  r10,   c92, r9.x, r10 ; + sky * t
//     mad  r10,   c91, 1.0, r10  ; + ground, unconditionally
//     mul  r8,    r10, c69
//     add  r8,    r8,  c68       ; + ambient
//     mul  oD0,   r8,  c72
//
// Three things this had wrong before, all of them visible:
//
//   * c95 was negated. The game dots the normal against it directly, so
//     negating it left every upward-facing surface - the whole pitch - with
//     zero direct light and therefore no shadow at all.
//   * the hemisphere was a mix() between ground and sky. The game *adds*
//     ground unconditionally and adds sky scaled by the blend, which is a
//     brighter and flatter result.
//   * c69 was missing entirely, so nothing was scaled down before ambient
//     was added, and the whole image came out washed out.
//
// c95 and c93 are deliberately not normalised: they arrive about 0.128 long
// and the game dots them raw, so normalising them would multiply the
// directional term by eight.
// `skyVisibility` is the one term the game could not have computed. Its c92
// sky contribution assumes every surface has a clear view of the sky, because
// a rasteriser has no way to ask otherwise. A ray tracer can, and the answer
// is what puts a player back in contact with the grass instead of floating a
// shade above it. Pass 1.0 to get exactly what the game does.
//
// Only the sky term is attenuated: the ground term is not the sky, and the
// directional term already has a traced shadow of its own.
vec3 GameLighting(vec3 normal, float shadowVisibility, float skyVisibility)
{
    float nDotL = max(dot(normal, scene.lightDirection.xyz), 0.0);
    vec3  lit   = scene.lightColor.rgb * nDotL * shadowVisibility;

    float t = dot(normal, scene.hemisphereAxis.xyz) * 0.5 + 0.5;
    lit += scene.skyColor.rgb * t * skyVisibility;
    lit += scene.groundColor.rgb;

    return lit * scene.lightingScale.rgb + scene.ambient.rgb;
}

// ── Sampling ─────────────────────────────────────────────────────────────
//
// A hash rather than a stored sequence: every hit needs its own directions
// and there is nowhere to keep them.
//
// Deliberately not seeded on the frame. With no accumulation and no denoiser
// behind this, noise that changes every frame boils; noise that stands still
// reads as fixed grain on a surface, which is much less distracting and is
// what a temporal filter would want to start from anyway.
uint HashCombine(uint seed, uint v)
{
    v *= 0x9E3779B9u;
    v ^= v >> 15;
    return seed ^ (v + 0x9E3779B9u + (seed << 6) + (seed >> 2));
}

float RandomFloat(inout uint state)
{
    // PCG-style advance; the well-mixed bits are the top ones.
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}

// Van der Corput in base 2: the second half of a Hammersley sequence, which
// spreads a small number of samples far more evenly around the circle than
// independent draws do.
float RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Cosine-weighted around `n`, which is the distribution the sky term wants:
// the cosine falls out of the integral, so the estimator is the plain mean of
// the visibility, with no weighting to get wrong.
vec3 CosineHemisphere(vec3 n, float u1, float u2)
{
    // An orthonormal basis around n with no branch on which axis is safest
    // to cross with - Duff et al., which is exact even as n.z approaches -1.
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    const vec3  tangent   = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    const vec3  bitangent = vec3(b, s + n.y * n.y * a, -n.y);

    const float r   = sqrt(u1);
    const float phi = 6.2831853 * u2;
    return normalize(tangent * (r * cos(phi)) +
                     bitangent * (r * sin(phi)) +
                     n * sqrt(max(0.0, 1.0 - u1)));
}
