"""Picks a pixel in a recorded scene stream and says what the ray hits.

PESModHost --record writes the scene stream to disk; --replay renders it back
with no game running. This answers the other half of the question: not "what
does the frame look like" but "what *is* that thing".

It reproduces the renderer's ray generation, matrix-palette skinning and
alpha test on the CPU, straight from the recording, and reports the instance
a pixel resolves to along with its geometry, texture, flags and that
texture's alpha statistics.

    set STREAM=<recording>
    python tools/stream_pick.py <frame position> <x> <y>

The frame position matches PESModHost --skip, so rendering with
`--skip N --frames 1` and picking frame N+1 look at the same frame.

This exists because guessing from a single traced frame repeatedly cost a
game session per iteration and picked the wrong cause more than once. Two
artefacts were tracked down with it: quads painting a player's face across
the pitch (projected shadows sampled with the wrong texture address mode)
and, before that, the reasoning behind the coplanar pitch layers.
"""
import struct, sys
import numpy as np
from collections import Counter

import os
PATH  = os.environ.get("STREAM", r"C:\Program Files (x86)\PES 6 WeHellas Greek Superleague 2006-07\pesmod_stream.bin")
BUSY  = int(sys.argv[1]) if len(sys.argv) > 1 else 87
PX    = int(sys.argv[2]) if len(sys.argv) > 2 else 370
PY_   = int(sys.argv[3]) if len(sys.argv) > 3 else 330
W, H  = 960, 720

data = open(PATH, 'rb').read()
GEO_FMT = '<QIIIIII IIIIIII'
GEO_SIZE, TEX_SIZE, INST_SIZE = 64, 32, 184

geo, frames, texdb = {}, [], {}
cur, frameNo = None, 0
off = 0
while off + 8 <= len(data):
    t, ln = struct.unpack_from('<II', data, off)
    if ln < 8 or off + ln > len(data): break
    b = off + 8
    if t == 4:
        (gid,kind,stride,vcount,icount,istride,chash,
         uvo,nrmo,colo,bones,bidx,bwt,_p) = struct.unpack_from(GEO_FMT, data, b)
        vb = b + GEO_SIZE
        vbytes, ibytes = vcount*stride, icount*istride
        geo[gid] = (dict(stride=stride, vcount=vcount, icount=icount,
                         istride=istride, bones=bones, bidx=bidx, bwt=bwt,
                         uv=uvo),
                    data[vb:vb+vbytes], data[vb+vbytes:vb+vbytes+ibytes])
    elif t == 5:
        tid,fmt,w,h,mips,payload = struct.unpack_from('<QIIIII', data, b)
        if len(data) >= b+32+w*h*4:
            texdb[tid] = (w,h,data[b+32:b+32+w*h*4])
    elif t == 1:
        frameNo = struct.unpack_from('<Q', data, b)[0]; cur = []
    elif t == 3 and cur is not None:
        gid, texid, _n = struct.unpack_from('<QQQ', data, b)
        clip = np.frombuffer(data, dtype='<f4', count=16, offset=b+24).reshape(4,4).astype(np.float64)
        flags, palRegs = struct.unpack_from('<II', data, b+168)
        scale = struct.unpack_from('<f', data, b+176)[0]
        pal = None
        if palRegs:
            pal = np.frombuffer(data, dtype='<f4', count=palRegs*4,
                                offset=b+INST_SIZE).reshape(palRegs,4).astype(np.float64)
        cur.append((gid, texid, clip, flags, pal, scale))
    elif t == 2 and cur is not None:
        frames.append((frameNo, cur)); cur = None
    off += ln

# BUSY is a frame *position*, matching the host's --skip semantics.
frameIndex, instances = frames[min(BUSY, len(frames)-1)]
print('frame %d: %d instances' % (frameIndex, len(instances)))

dom = Counter(i[2].tobytes() for i in instances).most_common(1)[0][0]
VP = np.frombuffer(dom, dtype=np.float64).reshape(4,4)
invVP = np.linalg.inv(VP)

# The renderer's ray generation, verbatim.
ndc = np.array([(PX + 0.5)/W*2.0 - 1.0, 1.0 - (PY_ + 0.5)/H*2.0])
def unproject(depth):
    h = np.array([ndc[0], ndc[1], depth, 1.0]) @ invVP
    return h[:3]/h[3]
origin = unproject(0.0)
direction = unproject(1.0) - origin
direction /= np.linalg.norm(direction)

def skinned_positions(d, vbytes, pal, scale):
    n, st = d['vcount'], d['stride']
    P = np.frombuffer(vbytes, dtype='<f4', count=n*st//4)
    P = np.lib.stride_tricks.as_strided(P, shape=(n,3), strides=(st,4)).astype(np.float64)
    if not d['bones'] or pal is None:
        return P
    raw = np.frombuffer(vbytes, dtype=np.uint8, count=n*st).reshape(n, st)
    idx = raw[:, d['bidx']:d['bidx']+4]
    wt  = raw[:, d['bwt']:d['bwt']+4] if d['bwt'] != 0xFFFFFFFF else None
    out = np.zeros((n,3))
    for b in range(d['bones']):
        c = 2-b if b < 3 else 3
        rows = (idx[:,c]/255.0*scale + 0.5).astype(int)
        w = (wt[:,c]/255.0) if wt is not None else np.ones(n)
        ok = (rows+2 < len(pal)) & (w > 0)
        for v in np.nonzero(ok)[0]:
            m = pal[rows[v]:rows[v]+3]
            p4 = np.array([P[v,0],P[v,1],P[v,2],1.0])
            out[v] += w[v]*np.array([p4@m[0], p4@m[1], p4@m[2]])
    return out

def tri_indices(d, ib):
    if d['icount']:
        dt = '<u4' if d['istride']==4 else '<u2'
        return np.frombuffer(ib, dtype=dt).astype(np.int64)[:d['icount']].reshape(-1,3)
    return np.arange(d['vcount']//3*3, dtype=np.int64).reshape(-1,3)

best = None
for n, (gid, texid, clip, flags, pal, scale) in enumerate(instances):
    if flags & 0x40:            # kInstanceNoDepthWrite: not in the TLAS
        continue
    g = geo.get(gid)
    if not g: continue
    d, vbytes, ib = g
    if d['vcount'] == 0 or d['stride'] < 12: continue

    P = skinned_positions(d, vbytes, pal, scale)
    Wm = clip @ invVP
    wp = (np.hstack([P, np.ones((len(P),1))]) @ Wm)[:, :3]

    tri = tri_indices(d, ib)
    tri = tri[(tri < len(wp)).all(axis=1)]
    if not len(tri): continue

    v0, v1, v2 = wp[tri[:,0]], wp[tri[:,1]], wp[tri[:,2]]
    e1, e2 = v1-v0, v2-v0
    pv = np.cross(direction, e2)
    det = (e1*pv).sum(1)
    live = np.abs(det) > 1e-12
    if not live.any(): continue
    inv = np.zeros_like(det); inv[live] = 1.0/det[live]
    tv = origin - v0
    u = (tv*pv).sum(1)*inv
    qv = np.cross(tv, e1)
    vv = (direction*qv).sum(1)*inv
    tt = (e2*qv).sum(1)*inv
    hit = live & (u>=-1e-6) & (vv>=-1e-6) & (u+vv<=1+1e-6) & (tt>1e-4)
    if not hit.any(): continue

    # The renderer's any-hit shader ignores a hit whose texture alpha is
    # below the epsilon, so the picker has to as well or it reports a surface
    # the ray actually passes straight through.
    blended = (flags & 0x3) != 0        # alpha blend or alpha test
    order = np.argsort(np.where(hit, tt, np.inf))
    for k in order:
        if not hit[k]: break
        if blended and texid in texdb and d['uv'] != 0xFFFFFFFF:
            tw, th, tpx = texdb[texid]
            ta = np.frombuffer(tpx, dtype=np.uint8).reshape(th, tw, 4)[:, :, 3]
            base = d['uv'] // 4
            fl = np.frombuffer(vbytes, dtype='<f4', count=d['vcount']*d['stride']//4)
            uvs = np.lib.stride_tricks.as_strided(fl[base:], shape=(d['vcount'],2),
                                                  strides=(d['stride'],4))
            i0, i1, i2 = tri[k]
            b1, b2 = u[k], vv[k]
            uvh = (1-b1-b2)*uvs[i0] + b1*uvs[i1] + b2*uvs[i2]
            sx = int(np.clip(uvh[0]*tw, 0, tw-1)) if True else 0
            sy = int(np.clip(uvh[1]*th, 0, th-1))
            if ta[sy, sx] < 1:
                continue           # the renderer would step over this one
        if best is None or tt[k] < best[0]:
            best = (tt[k], n, gid, texid, flags, d, len(tri))
        break

if best is None:
    print('the ray hit nothing at (%d, %d)' % (PX, PY_))
else:
    t, n, gid, texid, flags, d, ntri = best
    print()
    print('pixel (%d, %d) hits instance %d at t = %.1f' % (PX, PY_, n, t))
    print('  geometry   %016X' % gid)
    print('  texture    %d' % texid)
    print('  flags      0x%X' % flags)
    print('  stride %d, %d verts, %d tris, bones %d' %
          (d['stride'], d['vcount'], ntri, d['bones']))
    if texid in texdb:
        w,h,px = texdb[texid]
        a = np.frombuffer(px, dtype=np.uint8).reshape(h,w,4)[:,:,3]
        rgb = np.frombuffer(px, dtype=np.uint8).reshape(h,w,4)[:,:,:3].astype(float)
        print('  texture %dx%d  alpha min %d mean %.0f max %d | frac 0<a<255: %.0f%%'
              % (w, h, a.min(), a.mean(), a.max(),
                 100.0*((a>0)&(a<255)).mean()))
        print('  mean bgr %s' % np.round(rgb.mean(axis=(0,1)),0))
