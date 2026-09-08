"""Shader Model 1.x disassembler for PESMod renderer captures.

pes6.exe assembles its vs.1.1 / ps.1.x shaders at runtime through the
statically linked D3DX8 assembler, so the compiled bytecode exists nowhere on
disk. PESMod's capture layer dumps it to <capture>/shaders/*.bin; this turns
those dumps back into readable assembly.

    python tools/sm1dis.py <capture>/frame_XXXXXX/shaders/*.bin

Reading the vertex shaders is how the transform layout was established: for
this game `m4x4 oPos, v0, c58` places the combined world-view-projection
matrix at constant registers c58..c61, which is what the scene builder needs
in order to place geometry. See docs/RENDERER.md.
"""
import struct, sys, os

OPS = {
 0:('nop',0,0), 1:('mov',1,1), 2:('add',1,2), 3:('sub',1,2), 4:('mad',1,3),
 5:('mul',1,2), 6:('rcp',1,1), 7:('rsq',1,1), 8:('dp3',1,2), 9:('dp4',1,2),
 10:('min',1,2), 11:('max',1,2), 12:('slt',1,2), 13:('sge',1,2),
 14:('exp',1,1), 15:('log',1,1), 16:('lit',1,1), 17:('dst',1,2),
 18:('lrp',1,3), 19:('frc',1,1),
 20:('m4x4',1,2), 21:('m4x3',1,2), 22:('m3x4',1,2), 23:('m3x3',1,2), 24:('m3x2',1,2),
 32:('pow',1,2), 33:('crs',1,2), 34:('sgn',1,3), 35:('abs',1,1), 36:('nrm',1,1),
 78:('expp',1,1), 79:('logp',1,1),
 64:('texcoord',1,0), 65:('texkill',1,0), 66:('tex',1,0),
 67:('texbem',1,1), 68:('texbeml',1,1), 69:('texreg2ar',1,1),
 70:('texreg2gb',1,1), 71:('texm3x2pad',1,1), 72:('texm3x2tex',1,1),
 73:('texm3x3pad',1,1), 74:('texm3x3tex',1,1), 76:('texm3x3spec',1,2),
 77:('texm3x3vspec',1,1), 80:('cnd',1,3), 81:('def',1,0),
 82:('texreg2rgb',1,1), 83:('texdp3tex',1,1), 84:('texm3x2depth',1,1),
 85:('texdp3',1,1), 86:('texm3x3',1,1), 87:('texdepth',1,0),
 88:('cmp',1,3), 89:('bem',1,2),
}
RT_VS = {0:'r',1:'v',2:'c',3:'a',4:'oRAST',5:'oD',6:'oT'}
RT_PS = {0:'r',1:'v',2:'c',3:'t',4:'oRAST',5:'oD',6:'oT'}
RAST = {0:'oPos',1:'oFog',2:'oPts'}

def regname(tok, is_ps):
    num = tok & 0x7FF
    rt  = (tok >> 28) & 0x7
    table = RT_PS if is_ps else RT_VS
    name = table.get(rt, '?%d' % rt)
    if name == 'oRAST':
        return RAST.get(num, 'oRAST%d' % num)
    if name in ('oD','oT'):
        return '%s%d' % (name, num)
    return '%s%d' % (name, num)

def writemask(tok):
    m = (tok >> 16) & 0xF
    if m == 0xF or m == 0: return ''
    return '.' + ''.join(c for c,b in zip('xyzw',(1,2,4,8)) if m & b)

def swizzle(tok):
    sw = (tok >> 16) & 0xFF
    comps = [(sw >> (i*2)) & 3 for i in range(4)]
    if comps == [0,1,2,3]: return ''
    s = ''.join('xyzw'[c] for c in comps)
    if len(set(s)) == 1: return '.' + s[0]
    return '.' + s

def srcmod(tok, body):
    mod = (tok >> 24) & 0xF
    return {0:body, 1:'-'+body, 2:body+'_bias', 3:'-'+body+'_bias',
            4:'1-'+body, 5:body+'_x2', 6:'-'+body+'_x2',
            8:body+'_dz', 10:body+'_dw', 11:'abs('+body+')',
            13:'-abs('+body+')', 14:'!'+body}.get(mod, body)

def disasm(path):
    data = open(path,'rb').read()
    toks = struct.unpack('<%dI' % (len(data)//4), data)
    ver = toks[0]
    is_ps = (ver >> 16) == 0xFFFF
    kind = 'ps' if is_ps else 'vs'
    print('  %s.%d.%d   (%d tokens, %d bytes)' %
          (kind, (ver>>8)&0xFF, ver&0xFF, len(toks), len(data)))
    i = 1
    while i < len(toks):
        t = toks[i]
        if t == 0x0000FFFF: break
        if (t & 0xFFFF) == 0xFFFE:           # comment block
            i += 1 + ((t >> 16) & 0x7FFF); continue
        op = t & 0xFFFF
        name, ndst, nsrc = OPS.get(op, ('op%d'%op, 1, 2))
        i += 1
        parts = []
        if name == 'def':
            d = toks[i]; i += 1
            vals = struct.unpack('<4f', struct.pack('<4I', *toks[i:i+4])); i += 4
            print('    def %s, %g, %g, %g, %g' % (regname(d,is_ps), *vals)); continue
        if ndst:
            d = toks[i]; i += 1
            parts.append(regname(d,is_ps) + writemask(d))
        for _ in range(nsrc):
            if i >= len(toks): break
            s = toks[i]; i += 1
            parts.append(srcmod(s, regname(s,is_ps) + swizzle(s)))
        print('    %-6s %s' % (name, ', '.join(parts)))

for p in sys.argv[1:]:
    print('===== %s =====' % os.path.basename(p))
    try: disasm(p)
    except Exception as e: print('  ERROR:', e)
    print()
