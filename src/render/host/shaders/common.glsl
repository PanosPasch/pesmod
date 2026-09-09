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
    uint     uvOffset;      // bytes into a vertex, to its float2 UV
    uint     indexStride;   // 2 or 4; 0 means non-indexed
    uint     textureSlot;
    vec4     baseColor;
};

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevel;
layout(binding = 1, set = 0, rgba8) uniform image2D outputImage;
layout(binding = 2, set = 0) uniform SceneBlock { SceneUniforms scene; };

// Every texture the scene could hit, reachable at once. Slot 0 is a 1x1
// white texture, so an untextured surface takes the same path with no branch.
layout(binding = 3, set = 0) uniform sampler2D textures[];

layout(binding = 4, set = 0, std430) readonly buffer InstanceBlock
{
    InstanceRecord instances[];
};

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

// Below this the surface is treated as absent rather than shaded. Chosen
// against the game's own pitch layers: of the seven coplanar draws covering
// the pitch, the two that are pure noise in a ray tracer have maximum alphas
// of 0.14 and 0.25, while the grass base is 1.0 everywhere.
const float kAlphaCutoff = 0.5;

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
