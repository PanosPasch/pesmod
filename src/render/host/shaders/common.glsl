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

    vec4 params;            // x = shadow ray max distance, y = exposure
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
    uint     _pad1;
};

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
    return bary.x * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.x)
         + bary.y * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.y)
         + bary.z * VertexUv(verts, rec.vertexStride, rec.uvOffset, tri.z);
}

// Below this a texel is treated as absent rather than composited. It is
// deliberately near zero: partial alpha is now composited by the ray
// generation rather than tested away, and the only thing worth rejecting
// outright is a texel that would contribute nothing while consuming one of
// the peeling layers. Of the game's seven coplanar pitch layers, two are
// entirely below this.
// Matches SceneIPC::kNoVertexAttribute: the offset a geometry reports for
// an attribute it does not have.
const uint kNoVertexAttribute = 0xFFFFFFFFu;

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
vec3 GameLighting(vec3 normal, float shadowVisibility)
{
    float nDotL = max(dot(normal, scene.lightDirection.xyz), 0.0);
    vec3  lit   = scene.lightColor.rgb * nDotL * shadowVisibility;

    float t = dot(normal, scene.hemisphereAxis.xyz) * 0.5 + 0.5;
    lit += scene.skyColor.rgb * t;
    lit += scene.groundColor.rgb;

    return lit * scene.lightingScale.rgb + scene.ambient.rgb;
}
