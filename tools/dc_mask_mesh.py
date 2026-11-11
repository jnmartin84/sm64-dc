#!/usr/bin/env python3
"""Polygon versions of the screen-transition masks (star, circle, Mario, Bowser).

On the N64 a mask is one textured square. On the Dreamcast that square is a blended layer over the
whole picture, in every tile the PVR draws, and twice as many tiles with FSAA, though almost all of
it is either solid or empty. So each mask becomes:

  cover     the solid part as triangles: the square minus the outline of the shape, plus the islands
            inside it (Bowser's eyes and teeth). Drawn in the OP list.
  feather   a ring from that outline out into the hole, alpha 255 to 0: the soft edge, in TR.

The hole itself is then not drawn at all. The outline is an alpha contour of the texture, and the
ring ends where the texture's alpha runs out, at most a few texels out. Its outer vertices keep
the texture's alpha where a thin gap between two walls never gets clear.

    tools/dc_mask_mesh.py [--preview OUT.png]

writes src/game/screen_transition_mesh.h. Needs numpy, scipy, Pillow and contourpy (matplotlib's
contour generator). The output is checked in, so a build needs none of them.
"""
import argparse
import contourpy
import numpy as np
from PIL import Image
from scipy import ndimage

OUT = 'src/game/screen_transition_mesh.h'
TEX = 'textures/segment2/segment2.%s.ia8.png'

# One unit of the mesh's coordinates: the square's half width is ONE (the matrix scales it to the
# transition's radius).
ONE = 8192
UPSAMPLE = 4      # the alpha field has 16 levels on 64x64; contours are taken from it enlarged
SMOOTH = 0.45     # texels of blur, for those 16 levels
SIMPLIFY = 0.20   # outline tolerance in texels
FEATHER_LO = 12   # alpha where the feather has reached the clear hole
BATCH = 32        # vertices in one gSPVertex

# Outline alpha and widest ring in texels, picked against the texture: 3.5-8% RMS from it, with the
# ring 9-18% of the square. A wider ring comes closer and costs fill in proportion.
#         name      texture  mirrored  outline  ring
SHAPES = [('star',   '0F458', True,  210, 2.25),
          ('circle', '0FC58', True,  210, 3.0),
          ('mario',  '10458', False, 210, 1.75),
          ('bowser', '11458', True,  190, 1.75)]


def load_field(tex, mirrored):
    """The mask's alpha as the square shows it, 255 = solid, with the mesh units per texel and the
    texels from the middle of the 64 x 64 image to the middle of the square.

    A mirrored texture holds the right half: tx -31..31 maps to -1..1, with column 0's left edge in
    the middle. Mario holds all of it: tx 0..63 maps to -1..1. On both ty 0..63 maps to 1..-1."""
    a = np.array(Image.open(TEX % tex).convert('LA'))[..., 1].astype(float)
    if mirrored:
        return np.concatenate([a[:, ::-1], a], axis=1), 1 / 31.0, 1 / 31.5, 0.0
    return a, 1 / 31.5, 1 / 31.5, 0.5


class Field:
    """The alpha field, bilinear, in mesh coordinates (-1..1, y up), padded with solid."""

    def __init__(self, a, sx, sy, ox):
        pad = 2
        a = np.pad(a, pad, constant_values=255)
        big = ndimage.zoom(a, UPSAMPLE, order=1, grid_mode=True, mode='nearest')
        self.z = ndimage.gaussian_filter(big, SMOOTH * UPSAMPLE, mode='nearest')
        h, w = big.shape
        self.x = (np.arange(w) + 0.5) / UPSAMPLE - pad - 32.0
        self.y = (np.arange(h) + 0.5) / UPSAMPLE - pad - 32.0
        self.sx, self.sy, self.ox = sx, sy, ox

    def contours(self, level):
        gen = contourpy.contour_generator(x=self.x, y=self.y, z=self.z)
        out = []
        for line in gen.lines(level):
            if len(line) < 8 or not np.allclose(line[0], line[-1]):
                continue
            out.append(self.to_mesh(line[:-1]))
        return out

    def to_mesh(self, p):
        return np.stack([(p[:, 0] + self.ox) * self.sx, -(p[:, 1] + 0.5) * self.sy], axis=1)

    def alpha(self, p):
        """Alpha at mesh points p (n x 2)."""
        col = (p[:, 0] / self.sx - self.ox - self.x[0]) * UPSAMPLE
        row = (-p[:, 1] / self.sy - 0.5 - self.y[0]) * UPSAMPLE
        return ndimage.map_coordinates(self.z, [row, col], order=1, mode='nearest')


def simplify(loop, tol):
    """Douglas-Peucker on a closed loop."""
    start = int(np.argmax(np.sum((loop - loop[0]) ** 2, axis=1)))
    loop = np.roll(loop, -start, axis=0)
    far = int(np.argmax(np.sum((loop - loop[0]) ** 2, axis=1)))

    def rec(a, b):
        if b <= a + 1:
            return []
        d = loop[b] - loop[a]
        n = np.hypot(*d)
        rel = loop[a + 1:b] - loop[a]
        dist = np.abs(rel[:, 0] * d[1] - rel[:, 1] * d[0]) / (n if n else 1)
        i = int(np.argmax(dist))
        if dist[i] <= tol:
            return []
        m = a + 1 + i
        return rec(a, m) + [m] + rec(m, b)

    keep = [0] + rec(0, far) + [far] + rec(far, len(loop) - 1) + [len(loop) - 1]
    return loop[keep]


def area(p):
    x, y = p[:, 0], p[:, 1]
    return 0.5 * np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y)


def inside(pt, poly):
    x, y = pt
    px, py = poly[:, 0], poly[:, 1]
    qx, qy = np.roll(px, -1), np.roll(py, -1)
    cross = (py > y) != (qy > y)
    return np.count_nonzero(x < (qx - px)[cross] * (y - py[cross]) / (qy - py)[cross] + px[cross]) & 1


def cross2(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def in_tri(p, a, b, c):
    return cross2(a, b, p) >= 0 and cross2(b, c, p) >= 0 and cross2(c, a, p) >= 0


def merge_hole(outer, hole):
    """Joins a clockwise hole to a counter-clockwise polygon through a bridge between two vertices
    that can see each other."""
    outer_list = [tuple(p) for p in outer]
    hole_list = [tuple(p) for p in hole]
    best = None
    i = max(range(len(hole_list)), key=lambda k: hole_list[k][0])
    h = np.array(hole_list[i])
    order = sorted(range(len(outer_list)), key=lambda k: np.sum((np.array(outer_list[k]) - h) ** 2))

    def blocked(a, b, loop):
        n = len(loop)
        for k in range(n):
            c, d = loop[k], loop[(k + 1) % n]
            if a in (c, d) or b in (c, d):
                continue
            if cross2(a, b, c) * cross2(a, b, d) < 0 and cross2(c, d, a) * cross2(c, d, b) < 0:
                return True
        return False

    for j in order:
        if not blocked(outer_list[j], tuple(h), outer_list) and \
           not blocked(outer_list[j], tuple(h), hole_list):
            best = j
            break
    if best is None:
        raise SystemExit('no bridge to a hole')
    rotated = hole_list[i:] + hole_list[:i]
    return np.array(outer_list[:best + 1] + rotated + [rotated[0]] + outer_list[best:])


def triangulate(poly):
    """Ear clipping, shortest diagonal first. poly: counter-clockwise, simple apart from bridges.
    Returns triangles of points."""
    pts = [tuple(p) for p in poly]
    tris = []
    idx = list(range(len(pts)))
    while len(idx) > 3:
        best = None
        best_len = None
        n = len(idx)
        for k in range(n):
            ia, ib, ic = idx[k - 1], idx[k], idx[(k + 1) % n]
            a, b, c = pts[ia], pts[ib], pts[ic]
            if cross2(a, b, c) <= 0:
                continue
            ok = True
            for m in idx:
                if m in (ia, ib, ic):
                    continue
                p = pts[m]
                if p in (a, b, c):
                    continue
                if in_tri(p, a, b, c):
                    ok = False
                    break
            if not ok:
                continue
            d = (a[0] - c[0]) ** 2 + (a[1] - c[1]) ** 2
            if best is None or d < best_len:
                best, best_len = k, d
        if best is None:
            # only degenerate corners left (the two sides of a bridge)
            k = min(range(n), key=lambda k: abs(cross2(pts[idx[k - 1]], pts[idx[k]], pts[idx[(k + 1) % n]])))
            del idx[k]
            continue
        n = len(idx)
        tris.append((pts[idx[best - 1]], pts[idx[best]], pts[idx[(best + 1) % n]]))
        del idx[best]
    if cross2(pts[idx[0]], pts[idx[1]], pts[idx[2]]) > 0:
        tris.append(tuple(pts[i] for i in idx))
    return tris


def cover_triangles(loops):
    """The solid part of the square: the outer contours are holes in the square, the ones inside
    them islands (and holes in those, and so on)."""
    depth = [sum(inside(l[0], o) for o in loops if o is not l) for l in loops]
    tris = []
    for i, loop in enumerate(loops):
        if depth[i] % 2 == 0:
            continue
        # An island, with the contours just inside it as its holes.
        poly = loop if area(loop) > 0 else loop[::-1]
        for j, sub in enumerate(loops):
            if depth[j] == depth[i] + 1 and inside(sub[0], loop):
                poly = merge_hole(poly, sub if area(sub) < 0 else sub[::-1])
        tris += triangulate(poly)
    square = np.array([(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)])
    for j, sub in enumerate(loops):
        if depth[j] == 0:
            square = merge_hole(square, sub if area(sub) < 0 else sub[::-1])
    return tris + triangulate(square)


def feather_strip(field, loop, widest):
    """Quads from the outline into the hole, as (p, q, p', q') rails with alphas."""
    prev, nxt = np.roll(loop, 1, axis=0), np.roll(loop, -1, axis=0)
    def unit(v):
        return v / np.maximum(np.hypot(v[:, 0], v[:, 1]), 1e-9)[:, None]
    e1, e2 = unit(loop - prev), unit(nxt - loop)
    n = unit(np.stack([-(e1[:, 1] + e2[:, 1]), e1[:, 0] + e2[:, 0]], axis=1))
    # Point the normals into the hole: the side where the alpha falls.
    step = 0.3 * field.sy
    if np.mean(field.alpha(loop + n * step) - field.alpha(loop - n * step)) > 0:
        n = -n
    texels = np.arange(0.1, widest + 1e-6, 0.1)
    outer = np.empty_like(loop)
    alpha = np.zeros(len(loop))
    for i in range(len(loop)):
        ray = loop[i] + n[i] * texels[:, None] * field.sy
        a = field.alpha(ray)
        stop, low = len(a) - 1, a[0]
        for k, v in enumerate(a):
            if v <= FEATHER_LO:
                stop = k
                break
            if v > low + 8:      # a wall ahead: the gap never clears, stop at its middle
                stop = k - 1
                alpha[i] = low
                break
            low = min(low, v)
        outer[i] = ray[max(stop, 0)]
    # At a sharp tip the rays of the two sides cross: where the outer rail runs against the outline,
    # its two vertices meet halfway.
    for _ in range(8):
        flipped = np.sum((np.roll(outer, -1, axis=0) - outer) * (nxt - loop), axis=1) < 0
        if not flipped.any():
            break
        for i in np.nonzero(flipped)[0]:
            j = (i + 1) % len(loop)
            outer[i] = outer[j] = (outer[i] + outer[j]) / 2
            alpha[i] = alpha[j] = max(alpha[i], alpha[j])
    return outer, alpha


def build(field, level, widest):
    loops = []
    for loop in field.contours(level):
        s = simplify(loop, SIMPLIFY * field.sy)
        if len(s) >= 4 and abs(area(s)) > (1.5 * field.sy) ** 2:
            loops.append(np.clip(s, -0.995, 0.995))
    tris = cover_triangles(loops)
    feather = []
    for loop in loops:
        outer, alpha = feather_strip(field, loop, widest)
        n = len(loop)
        for i in range(n):
            j = (i + 1) % n
            feather.append(((loop[i], 255), (outer[i], alpha[i]), (loop[j], 255), (outer[j], alpha[j])))
    return tris, feather


def to_s16(p):
    return (int(round(float(p[0]) * ONE)), int(round(float(p[1]) * ONE)))


def batches(tris):
    """Groups triangles of ((x, y), a) vertices into gSPVertex loads of up to BATCH vertices."""
    out, verts, cur = [], {}, []
    for tri in tris:
        new = [v for v in dict.fromkeys(tri) if v not in verts]
        if len(verts) + len(new) > BATCH:
            out.append((list(verts), cur))
            verts, cur = {}, []
            new = list(dict.fromkeys(tri))
        for v in new:
            verts[v] = len(verts)
        cur.append(tuple(verts[v] for v in tri))
    if cur:
        out.append((list(verts), cur))
    return out


def emit_list(f, name, tris, tag=None):
    b = batches(tris)
    f.write('static const Vtx %s_vtx[] = {\n' % name)
    for verts, _ in b:
        for (x, y), a in verts:
            f.write('    {{{%6d, %6d, -1}, 0, {0, 0}, {0, 0, 0, %3d}}},\n' % (x, y, a))
    f.write('};\n\nconst Gfx %s[] = {\n' % name)
    if tag:
        f.write('    gsDPNoOpTag(%s),\n' % tag)
    off = 0
    for verts, cur in b:
        f.write('    gsSPVertex(%s_vtx + %d, %d, 0),\n' % (name, off, len(verts)))
        off += len(verts)
        for k in range(0, len(cur) - 1, 2):
            a, c = cur[k], cur[k + 1]
            f.write('    gsSP2Triangles(%2d, %2d, %2d, 0x0, %2d, %2d, %2d, 0x0),\n' % (a + c))
        if len(cur) & 1:
            f.write('    gsSP1Triangle(%2d, %2d, %2d, 0x0),\n' % cur[-1])
    f.write('    gsSPEndDisplayList(),\n};\n\n')
    return off


def facing(tris):
    """Counter-clockwise, as G_CULL_BACK wants them, without the ones the rounding flattened."""
    out = []
    for t in tris:
        (a, _), (b, _), (c, _) = t
        d = cross2(a, b, c)
        if d:
            out.append(t if d > 0 else (t[0], t[2], t[1]))
    return out


def mesh_lists(tris, feather):
    cover = [tuple((to_s16(p), 255) for p in t) for t in tris]
    ring = []
    for (p, pa), (q, qa), (r, ra), (s, sa) in feather:
        p, q, r, s = (to_s16(p), int(pa)), (to_s16(q), int(qa)), (to_s16(r), int(ra)), (to_s16(s), int(sa))
        ring.append((p, q, s))
        ring.append((p, s, r))
    return facing(cover), facing(ring)


def tri_area(t):
    (a, _), (b, _), (c, _) = t
    return abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * 0.5 / (ONE * ONE)


def render(a, sx, sy, ox, cover, ring, size):
    """What the square shows, 1 = solid: the texture, bilinear, and the mesh."""
    ys, xs = np.mgrid[0:size, 0:size]
    nx = (xs + 0.5) / size * 2 - 1
    ny = 1 - (ys + 0.5) / size * 2
    col = nx / sx - ox + 31.5
    rw = -ny / sy - 0.5 + 31.5
    orig = ndimage.map_coordinates(a, [rw, col], order=1, mode='nearest') / 255
    sim = np.zeros((size, size))
    for tris in (cover, ring):
        for t in tris:
            p = np.array([v[0] for v in t], float) / ONE
            al = np.array([v[1] for v in t], float) / 255
            px = (p[:, 0] + 1) / 2 * size
            py = (1 - p[:, 1]) / 2 * size
            x0, x1 = max(int(px.min()), 0), min(int(px.max()) + 2, size)
            y0, y1 = max(int(py.min()), 0), min(int(py.max()) + 2, size)
            if x0 >= x1 or y0 >= y1:
                continue
            gy, gx = np.mgrid[y0:y1, x0:x1] + 0.5
            d = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2])
            if abs(d) < 1e-9:
                continue
            l0 = ((py[1] - py[2]) * (gx - px[2]) + (px[2] - px[1]) * (gy - py[2])) / d
            l1 = ((py[2] - py[0]) * (gx - px[2]) + (px[0] - px[2]) * (gy - py[2])) / d
            l2 = 1 - l0 - l1
            m = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
            val = l0 * al[0] + l1 * al[1] + l2 * al[2]
            reg = sim[y0:y1, x0:x1]
            reg[m] = reg[m] + (1 - reg[m]) * val[m]
    return orig, sim


def preview(path, results):
    """Original against mesh over a grey background, as the screen would show it."""
    size = 512
    tile = Image.new('RGB', (size * 2 + 8, (size + 8) * len(results)), (255, 0, 255))
    ys, xs = np.mgrid[0:size, 0:size]
    for row, (name, a, sx, sy, ox, cover, ring) in enumerate(results):
        for k, img in enumerate(render(a, sx, sy, ox, cover, ring, size)):
            bg = 0.75 + 0.2 * (((xs // 32) + (ys // 32)) & 1)
            v = (bg * (1 - img) * 255).astype('uint8')
            tile.paste(Image.fromarray(v).convert('RGB'), (k * (size + 8), row * (size + 8)))
    tile.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--preview')
    args = ap.parse_args()
    results = []
    with open(OUT, 'w') as f:
        f.write('// Generated by tools/dc_mask_mesh.py from the transition textures.\n')
        f.write('// The square is -%d..%d on both axes.\n#define MASK_MESH_ONE %d\n\n' % (ONE, ONE, ONE))
        for name, tex, mirrored, level, widest in SHAPES:
            a, sx, sy, ox = load_field(tex, mirrored)
            field = Field(a, sx, sy, ox)
            tris, feather = build(field, level, widest)
            cover, ring = mesh_lists(tris, feather)
            nc = emit_list(f, 'dl_mask_%s_cover' % name, cover)
            nr = emit_list(f, 'dl_mask_%s_feather' % name, ring, 'DC_GFX_NO_COMPILE')
            ta = sum(tri_area(t) for t in ring) / 4
            ca = sum(tri_area(t) for t in cover) / 4
            print('%-7s cover %3d tris %3d verts %5.1f%%   feather %3d tris %3d verts %5.1f%% of the square'
                  % (name, len(cover), nc, ca * 100, len(ring), nr, ta * 100))
            results.append((name, a, sx, sy, ox, cover, ring))
    if args.preview:
        preview(args.preview, results)


if __name__ == '__main__':
    main()
