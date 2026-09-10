// shader_analysis.cpp - see shader_analysis.h

#include "shader_analysis.h"

#include <string.h>

namespace
{
    // ── Shader Model 1 token format ──────────────────────────────────────
    //
    // An instruction token carries its opcode in bits 0..15 and is followed
    // by its destination and source parameter tokens, one each. vs.1.1 does
    // not encode an instruction's length, so the walk needs an arity table -
    // and an opcode missing from that table has to stop the walk rather than
    // be skipped, because after one wrong length every token is misread.
    const uint32_t kOpcodeMask   = 0x0000FFFFu;
    const uint32_t kOpEnd        = 0x0000FFFFu;
    const uint32_t kOpComment    = 0x0000FFFEu;

    const uint32_t kOpMov  = 1u;
    const uint32_t kOpMad  = 4u;
    const uint32_t kOpM4x4 = 20u;
    const uint32_t kOpM4x3 = 21u;
    const uint32_t kOpDef  = 81u;

    // Register file, from bits 28..30 of a parameter token.
    const uint32_t kRegTemp   = 0u;   // rN
    const uint32_t kRegInput  = 1u;   // vN
    const uint32_t kRegConst  = 2u;   // cN
    const uint32_t kRegRaster = 4u;   // oPos (0), oFog (1), oPts (2)
    const uint32_t kRegTexOut = 6u;   // oTN

    const uint32_t kRasterPos = 0u;

    // The default source swizzle, .xyzw - components 0,1,2,3 at two bits
    // each. A single-component swizzle replicates, so .x is 0x00, .y 0x55,
    // .z 0xAA and .w 0xFF.
    const uint32_t kSwizzleIdentity = 0xE4u;

    inline uint32_t RegNumber(uint32_t tok) { return tok & 0x7FFu; }
    inline uint32_t RegType(uint32_t tok)   { return (tok >> 28) & 0x7u; }
    inline uint32_t Swizzle(uint32_t tok)   { return (tok >> 16) & 0xFFu; }

    // The component a replicating swizzle selects, or 4 when the swizzle
    // selects more than one.
    uint32_t ReplicatedComponent(uint32_t tok)
    {
        const uint32_t sw = Swizzle(tok);
        const uint32_t c  = sw & 3u;
        if (((c << 0) | (c << 2) | (c << 4) | (c << 6)) != sw) return 4u;
        return c;
    }

    // Source token count. Every destination is a single token.
    // Returns false for an opcode this walk has never been shown, which
    // stops the decode - see the note on arity above.
    bool SourceCount(uint32_t op, uint32_t& sources, uint32_t& dests)
    {
        dests = 1;
        switch (op)
        {
        case 0:                                     sources = 0; dests = 0; return true;  // nop
        case 1: case 6: case 7: case 14: case 15:
        case 16: case 19: case 35: case 36:
        case 78: case 79:                           sources = 1; return true;
        case 2: case 3: case 5: case 8: case 9:
        case 10: case 11: case 12: case 13:
        case 17: case 20: case 21: case 22:
        case 23: case 24: case 32: case 33:         sources = 2; return true;
        case 4: case 18: case 34: case 80: case 88: sources = 3; return true;
        default: return false;
        }
    }
}

namespace Capture { namespace ShaderAnalysis {

void Analyse(const std::vector<uint32_t>& fn, VertexShaderProgram& out)
{
    memset(&out, 0, sizeof(out));
    out.uvRegisterA = kNoRegister;
    out.uvRegisterB = kNoRegister;
    if (fn.size() < 2) return;

    // ── Pass one: index the instructions ─────────────────────────────────
    //
    // Recognising an idiom means looking at more than one instruction - the
    // texture transform spans two, and the morph accumulator is only
    // identifiable from the m4x4 that consumes it - so the tokens are walked
    // once into a flat list and matched afterwards.
    struct Insn
    {
        uint32_t op;
        uint32_t dest;
        uint32_t src[3];
        uint32_t sourceCount;
    };

    std::vector<Insn> code;
    code.reserve(64);

    size_t i = 1;
    while (i < fn.size())
    {
        const uint32_t tok = fn[i];
        if (tok == kOpEnd) break;

        const uint32_t op = tok & kOpcodeMask;
        if (op == kOpComment)
        {
            i += 1 + ((tok >> 16) & 0x7FFFu);
            continue;
        }

        uint32_t sources = 0, dests = 0;
        if (!SourceCount(op, sources, dests)) return;   // out.decoded stays false

        // `def cN, f, f, f, f` - one destination and four raw floats, which
        // are not parameter tokens and must not be read as registers.
        const uint32_t extra = (op == kOpDef) ? 4u : 0u;
        if (i + 1 + dests + sources + extra > fn.size()) return;

        Insn in;
        in.op          = op;
        in.dest        = dests ? fn[i + 1] : 0u;
        in.sourceCount = sources;
        for (uint32_t s = 0; s < 3; ++s)
            in.src[s] = (s < sources) ? fn[i + 1 + dests + s] : 0u;
        code.push_back(in);

        i += 1 + dests + sources + extra;
    }

    out.decoded = true;

    // ── Skinning: m4x3 against the constant file ─────────────────────────
    for (size_t k = 0; k < code.size(); ++k)
        if (code[k].op == kOpM4x3 && code[k].sourceCount >= 2 &&
            RegType(code[k].src[1]) == kRegConst)
            ++out.paletteTransforms;

    // ── The texture transform ────────────────────────────────────────────
    //
    // Matched on the last write to oT0, since that is the value the pixel
    // stage receives.
    size_t uvWrite = code.size();
    for (size_t k = 0; k < code.size(); ++k)
        if (RegType(code[k].dest) == kRegTexOut && RegNumber(code[k].dest) == 0u)
            uvWrite = k;

    if (uvWrite < code.size())
    {
        const Insn& w = code[uvWrite];

        // `mov oT0.xy, vN` - the UV reaches the pixel stage untouched.
        const bool passthrough = (w.op == kOpMov && w.sourceCount == 1 &&
                                  RegType(w.src[0]) == kRegInput);

        // `mad oT0.xy, vN.y, cB, rT` where rT came from
        // `mad rT.xy, vN.x, cA.xyyy, cA.zwww`.
        bool matched = false;
        if (!passthrough && w.op == kOpMad && w.sourceCount == 3 &&
            RegType(w.src[0]) == kRegInput &&
            ReplicatedComponent(w.src[0]) == 1u &&      // .y
            RegType(w.src[1]) == kRegConst &&
            RegType(w.src[2]) == kRegTemp)
        {
            const uint32_t accumulator = RegNumber(w.src[2]);
            for (size_t k = uvWrite; k-- > 0; )
            {
                const Insn& s = code[k];
                if (RegType(s.dest) != kRegTemp ||
                    RegNumber(s.dest) != accumulator) continue;

                if (s.op == kOpMad && s.sourceCount == 3 &&
                    RegType(s.src[0]) == kRegInput &&
                    ReplicatedComponent(s.src[0]) == 0u &&     // .x
                    RegType(s.src[1]) == kRegConst &&
                    RegType(s.src[2]) == kRegConst &&
                    RegNumber(s.src[1]) == RegNumber(s.src[2]) &&
                    RegNumber(s.src[0]) == RegNumber(w.src[0]))
                {
                    out.uvIsTransformed = true;
                    out.uvRegisterA     = RegNumber(s.src[1]);
                    out.uvRegisterB     = RegNumber(w.src[1]);
                    matched             = true;
                }
                break;     // only the nearest preceding write can be the one
            }
        }

        out.uvUnrecognised = !passthrough && !matched;
    }

    // ── Morph targets ────────────────────────────────────────────────────
    //
    // The accumulator is whichever temp register the position transform
    // consumes; a shader with no morphing feeds `m4x4 oPos, v0, c58` from
    // the input register directly, so there is nothing to find.
    size_t posWrite = code.size();
    for (size_t k = 0; k < code.size(); ++k)
        if (code[k].op == kOpM4x4 && RegType(code[k].dest) == kRegRaster &&
            RegNumber(code[k].dest) == kRasterPos)
            posWrite = k;

    if (posWrite >= code.size()) return;
    if (code[posWrite].sourceCount < 1) return;
    if (RegType(code[posWrite].src[0]) != kRegTemp) return;

    const uint32_t accumulator = RegNumber(code[posWrite].src[0]);

    // Seeded straight from stream 0's position, `mov rN, v0`. A skinned
    // shader seeds the same register from `m4x3 rN, v0, c0` instead, and
    // must not be read as a morph: its deltas are bone transforms, not
    // vertex streams.
    bool seeded = false;
    for (size_t k = 0; k < posWrite; ++k)
    {
        const Insn& s = code[k];
        if (RegType(s.dest) != kRegTemp || RegNumber(s.dest) != accumulator)
            continue;
        seeded = (s.op == kOpMov && s.sourceCount == 1 &&
                  RegType(s.src[0]) == kRegInput &&
                  RegNumber(s.src[0]) == 0u &&
                  Swizzle(s.src[0]) == kSwizzleIdentity);
        break;
    }
    if (!seeded) return;

    // `mad acc.xyz, vN, cM.<component>, acc`
    for (size_t k = 0; k < posWrite && out.morphCount < kMaxMorphTargets; ++k)
    {
        const Insn& s = code[k];
        if (s.op != kOpMad || s.sourceCount != 3) continue;
        if (RegType(s.dest) != kRegTemp || RegNumber(s.dest) != accumulator)
            continue;
        if (RegType(s.src[0]) != kRegInput)  continue;
        if (RegType(s.src[1]) != kRegConst)  continue;
        if (RegType(s.src[2]) != kRegTemp ||
            RegNumber(s.src[2]) != accumulator) continue;

        const uint32_t component = ReplicatedComponent(s.src[1]);
        if (component > 3u) continue;

        MorphTarget& m    = out.morph[out.morphCount++];
        m.inputRegister   = RegNumber(s.src[0]);
        m.weightRegister  = RegNumber(s.src[1]);
        m.weightComponent = component;
    }
}

}} // namespace Capture::ShaderAnalysis
