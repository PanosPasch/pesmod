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

# The camera, recovered the way AccelBuilder does: the eye is the world point
# projecting to clip x = y = w = 0.
A = np.stack([VP[:3,0], VP[:3,1], VP[:3,3]], axis=0)
CAM = np.linalg.solve(A, -np.array([VP[3,0], VP[3,1], VP[3,3]]))

# The renderer nudges blended instances toward the viewer by draw order so
# coplanar decals have a defined order. Without reproducing that here the
# picker reports a different frontmost surface than the renderer draws.
# Must match AccelBuilder::kDecalBias / kResumeFraction, which the host
# passes to the shaders in SceneUniforms::decal. RESUME_FRACTION is what
# keeps the peel from stepping past the surface a decal decorates: a
# whole step landed beyond it and the base was never composited.
DECAL_BIAS, MAX_STEPS, RESUME_FRACTION = 1.0e-5, 128, 0.25

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

def encloses_camera(g, clip):
    """Whether this draw's world bounds contain the eye - the builder's test
    for the sky, which is the one non-occluding draw kept out of the TLAS."""
    d, vbytes, _ib = g
    n, st = d['vcount'], d['stride']
    if n == 0 or st < 12:
        return False
    P = np.frombuffer(vbytes, dtype='<f4', count=n*st//4)
    P = np.lib.stride_tricks.as_strided(P, shape=(n, 3),
                                        strides=(st, 4)).astype(np.float64)
    wp = (np.hstack([P, np.ones((len(P), 1))]) @ (clip @ invVP))[:, :3]
    lo, hi = wp.min(0), wp.max(0)
    return bool(np.all(CAM >= lo - 1.0) and np.all(CAM <= hi + 1.0))


def tri_indices(d, ib):
    if d['icount']:
        dt = '<u4' if d['istride']==4 else '<u2'
        return np.frombuffer(ib, dtype=dt).astype(np.int64)[:d['icount']].reshape(-1,3)
    return np.arange(d['vcount']//3*3, dtype=np.int64).reshape(-1,3)

hits = []       # every surface the ray crosses, not just the first
decalOrder = 0
skipped_sky = 0
for n, (gid, texid, clip, flags, pal, scale) in enumerate(instances):
    # kInstanceNoDepthWrite no longer means "dropped". The builder puts these
    # in the TLAS on kMaskNonOccluding, where a primary ray sees them and a
    # shadow ray does not - only the sky, identified by its bounds containing
    # the camera, stays out. Skipping them here made this tool disagree with
    # the renderer about exactly the draws that were newly let in.
    if flags & 0x40:
        g0 = geo.get(gid)
        if g0 and encloses_camera(g0, clip):
            skipped_sky += 1
            continue
    g = geo.get(gid)
    if not g: continue
    d, vbytes, ib = g
    if d['vcount'] == 0 or d['stride'] < 12: continue

    P = skinned_positions(d, vbytes, pal, scale)
    Wm = (clip @ invVP).copy()

    if flags & 0x3:                       # blended: biased toward the viewer
        delta = CAM - Wm[3, :3]
        length = np.linalg.norm(delta)
        if length > 1e-3:
            steps = min(decalOrder, MAX_STEPS)
            Wm[3, :3] += delta / length * (steps * DECAL_BIAS * length)
        decalOrder += 1

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
    hitmask = live & (u>=-1e-6) & (vv>=-1e-6) & (u+vv<=1+1e-6) & (tt>1e-4)
    if not hitmask.any(): continue

    k = int(np.argmin(np.where(hitmask, tt, np.inf)))

    # Alpha at the hit, the way the any-hit shader reads it.
    alpha = 255
    if texid in texdb and d['uv'] != 0xFFFFFFFF:
        tw, th, tpx = texdb[texid]
        ta = np.frombuffer(tpx, dtype=np.uint8).reshape(th, tw, 4)[:, :, 3]
        base = d['uv'] // 4
        fl = np.frombuffer(vbytes, dtype='<f4', count=d['vcount']*d['stride']//4)
        uvs = np.lib.stride_tricks.as_strided(fl[base:], shape=(d['vcount'],2),
                                              strides=(d['stride'],4))
        i0, i1, i2 = tri[k]
        b1, b2 = u[k], vv[k]
        uvh = (1-b1-b2)*uvs[i0] + b1*uvs[i1] + b2*uvs[i2]
        sx = int(np.clip(uvh[0]*tw, 0, tw-1)); sy = int(np.clip(uvh[1]*th, 0, th-1))
        alpha = int(ta[sy, sx])

    hits.append((float(tt[k]), n, gid, texid, flags, d, len(tri), alpha))

hits.sort()
print()
print('surfaces along the ray at (%d, %d), nearest first:' % (PX, PY_))
print('%-10s %-6s %-7s %-8s %-7s %-7s %s' %
      ('t','inst','tex','flags','alpha','tris','geometry'))
for h in hits[:10]:
    t, n, gid, texid, flags, d, ntri, alpha = h
    print('%-10.1f %-6d %-7d 0x%-6X %-7d %-7d %016X' %
          (t, n, texid, flags, alpha, ntri, gid))

# ── What the ray generation would composite ─────────────────────────────
#
# Listing the surfaces is not the same as saying what comes out: the peeling
# loop walks them front to back with a running transmittance and gives up
# after kMaxLayers, dropping whatever is left. A pixel whose base surface
# sits past that limit renders dark for a reason no single hit explains,
# which is why this is spelled out rather than left to be inferred.
K_MAX_LAYERS = 12
print()
print('the peeling loop, front to back (kMaxLayers = %d):' % K_MAX_LAYERS)
transmittance = 1.0
consumed = 0
resume = 0.0
for layer, h in enumerate(hits):
    # Skipped for the same reason the renderer would skip it: the previous
    # layer pushed tmin past this one.
    if h[0] < resume:
        print('  layer %-2d inst %-4d tex %-6d at t=%.4f is INSIDE the resume '
              'epsilon (%.4f) and is stepped over'
              % (layer, h[1], h[3], h[0], resume))
        continue
    resume = h[0] + max(h[0] * DECAL_BIAS * RESUME_FRACTION, 1.0e-4)
    if layer >= K_MAX_LAYERS:
        print('  ... %d more surface(s) never reached; %.3f of the ray is '
              'still unaccounted for and is dropped'
              % (len(hits) - K_MAX_LAYERS, transmittance))
        break
    t, n, gid, texid, flags, d, ntri, alpha = h
    # The any-hit shader steps over a texel with no coverage at all.
    if alpha < 1:
        print('  layer %-2d skipped by the alpha test (inst %d, tex %d)'
              % (layer, n, texid))
        continue
    a = alpha / 255.0
    print('  layer %-2d inst %-4d tex %-6d alpha %.3f  contributes %.3f, '
          'transmittance %.3f -> %.3f'
          % (consumed, n, texid, a, transmittance * a,
             transmittance, transmittance * (1.0 - a)))
    transmittance *= (1.0 - a)
    consumed += 1
    if transmittance < 1.0 / 255.0:
        print('  opaque by layer %d; nothing behind it is visible' % consumed)
        break
else:
    if transmittance >= 1.0 / 255.0:
        print('  ran out of surfaces with %.3f transmittance left, so the '
              'background shows through' % transmittance)

if skipped_sky:
    print()
    print('(%d draw(s) kept out as sky: their bounds contain the camera)'
          % skipped_sky)

best = hits[0] if hits else None

if best is None:
    print('the ray hit nothing at (%d, %d)' % (PX, PY_))
else:
    t, n, gid, texid, flags, d, ntri, alpha = best
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
