// shader_analysis.h
//
// What a vs.1.1 shader does to a vertex, read from the game's own bytecode.
//
// pes6.exe assembles its shaders at runtime through the statically linked
// D3DX8 assembler, so the compiled form exists only in memory - the capture
// layer keeps a verbatim copy, and this reads it.
//
// Three things in those shaders change a vertex in ways nothing else can
// tell us about, and each of them was visible as a rendering bug for as long
// as this file did not exist:
//
//   • the matrix palette      - a skinned draw's vertices are bone-local
//   • the texture transform   - oT0 is an affine function of the input UV
//   • the morph targets       - position is stream 0 plus weighted deltas
//
// The disassembler in tools/sm1dis.py prints the same instructions; when a
// pattern here needs checking, run that over a capture's shaders/*.bin.
#pragma once

#include <stdint.h>
#include <vector>

namespace Capture { namespace ShaderAnalysis {

    // Six is what this game's declarations bind (streams 1..6 alongside
    // stream 0), and vs_0031 uses all of them: c81.xyzw then c82.xy.
    static const uint32_t kMaxMorphTargets = 8;
    static const uint32_t kNoRegister      = 0xFFFFFFFFu;

    // One `mad acc.xyz, vN, cM.<component>, acc` - a delta stream and the
    // constant component that weights it.
    struct MorphTarget
    {
        uint32_t inputRegister;    // vN; the declaration says which stream
        uint32_t weightRegister;   // cM
        uint32_t weightComponent;  // 0..3, i.e. x y z w
    };

    struct VertexShaderProgram
    {
        // False when the token walk hit something it could not size, in
        // which case every field below is meaningless. Nothing infers from a
        // partial decode: a wrong answer here moves geometry.
        bool     decoded;

        // ── Skinning ─────────────────────────────────────────────────────
        // `m4x3 rN, v0, c0` against a constant register is one palette
        // transform; the count is the number of influences per vertex.
        uint32_t paletteTransforms;

        // ── Texture coordinates ──────────────────────────────────────────
        // The game windows an atlas by transforming the UV in the shader:
        //
        //     mad  rN.xy,   v2.x, c75.xyyy, c75.zwww
        //     mad  oT0.xy,  v2.y, c76,      rN
        //
        // which is    oT0 = uv.x * cA.xy + uv.y * cB.xy + cA.zw
        //
        // Ignoring it draws the whole atlas across the quad, which is what
        // made every advertising hoarding show all its adverts at once
        // instead of scrolling one across.
        bool     uvIsTransformed;
        uint32_t uvRegisterA;      // cA: the u basis in .xy, the offset in .zw
        uint32_t uvRegisterB;      // cB: the v basis in .xy

        // oT0 was written by something this does not model. Reported rather
        // than guessed at, because a guessed texture transform is worse than
        // none: it moves texels on surfaces that are currently correct.
        bool     uvUnrecognised;

        // ── Morph targets ────────────────────────────────────────────────
        // Position is stream 0's vertex plus weighted deltas from the other
        // streams:
        //
        //     mov  r11,      v0
        //     mad  r11.xyz,  v3, c81.x, r11
        //     mad  r11.xyz,  v4, c81.y, r11
        //     ...
        //     m4x4 oPos,     r11, c58
        //
        // This is how the game opens a player's mouth, moves their eyes and
        // shapes their build. Reading stream 0 alone - which is all the
        // exporter did - freezes every face in its neutral pose.
        uint32_t    morphCount;
        MorphTarget morph[kMaxMorphTargets];
    };

    // Walks `fn` (a verbatim CreateVertexShader function, starting with its
    // version token) and fills `out`. Cheap: a linear pass over a couple of
    // hundred tokens, which is nothing beside the vertex buffer read that
    // follows it.
    void Analyse(const std::vector<uint32_t>& fn, VertexShaderProgram& out);

}} // namespace Capture::ShaderAnalysis
