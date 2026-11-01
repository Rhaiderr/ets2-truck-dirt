"""Draws near the truck's front wheels in the main G-buffer pass: distance of
each draw's origin from the configured wheel position and its rotation in the
truck's frame, to see what steering does to the wheel draws."""
import sys, math, collections
import capparse
from wheels import TRUCK, apply


def inv_apply(m, p):
    d = [p[i] - m[i][3] for i in range(3)]
    return tuple(m[0][c] * d[0] + m[1][c] * d[1] + m[2][c] * d[2] for c in range(3))


def rel_rot(a, b):
    return [[sum(a[k][i] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def run(path, vp=(4304, 1800)):
    head, draws = capparse.parse(path)
    g = [d for d in draws if d['targets'] == 7 and d['vp'] == vp and capparse.model_view(d)]
    pos = [(tuple(r[3] for r in capparse.model_view(d)), d) for d in g]
    best = None
    seen = set()
    for d in g:
        m = capparse.model_view(d)
        key = tuple(round(v, 3) for row in m for v in row)
        if key in seen:
            continue
        seen.add(key)
        hits = sum(1 for w in TRUCK if any(math.dist(apply(m, w), q) < 0.08 for q, _ in pos))
        if hits >= 3 and (best is None or hits > best[0]):
            best = (hits, m)
    print(path[-28:], 'steering', head.get('steer'), 'frame hits', best and best[0])
    if not best:
        return
    f = best[1]
    for wi in (0, 1):
        for q, d in pos:
            lp = inv_apply(f, q)
            dist = math.dist(lp, TRUCK[wi])
            if dist < 0.6:
                r = rel_rot(f, capparse.model_view(d))
                print('  wheel %d  d=%.3f  local=(%.2f %.2f %.2f)  r10=%.3f r00=%.3f r02=%.3f  draw %d %s' % (
                    wi, dist, lp[0], lp[1], lp[2], r[1][0], r[0][0], r[0][2], d['i'], d['label'][:30]))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p)
