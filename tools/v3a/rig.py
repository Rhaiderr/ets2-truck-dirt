"""Find the player's truck/trailer frames (wheel layout), then every draw of
the pass whose position falls in the vehicle's box, and check the
truck->trailer relative pose against the telemetry."""
import sys, math, collections
import capparse
from wheels import TRUCK, TRAILER, apply

BOX = {'truck': ((-1.6, 1.6), (-0.3, 4.2), (-3.2, 4.5)), 'trailer': ((-1.6, 1.6), (-0.3, 4.6), (-7.0, 8.0))}


def inv_apply(m, q):
    t = [m[r][3] for r in range(3)]
    d = [q[i] - t[i] for i in range(3)]
    # rotation part may carry scale; use the transpose divided by squared column norms
    return tuple(sum(m[r][c] * d[r] for r in range(3)) / sum(m[r][c] ** 2 for r in range(3)) for c in range(3))


def find_frame(g, wheels, tol=0.08):
    pos = [tuple(r[3] for r in capparse.model_view(d)) for d in g]
    best = None
    seen = set()
    for d in g:
        m = capparse.model_view(d)
        key = tuple(round(v, 3) for row in m for v in row)
        if key in seen:
            continue
        seen.add(key)
        hits = sum(1 for w in wheels if any(math.dist(apply(m, w), q) < tol for q in pos))
        if hits >= 3 and (not best or hits > best[0]):
            best = (hits, m)
    return best


def rot_world(h, p, r):
    # SCS: heading about +Y, pitch about +X, roll about +Z, angles in turns (heading 0 = -Z forward)
    import numpy as np
    a, b, c = (x * 2 * math.pi for x in (h, p, r))
    ry = np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]])
    rx = np.array([[1, 0, 0], [0, math.cos(b), -math.sin(b)], [0, math.sin(b), math.cos(b)]])
    rz = np.array([[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]])
    return ry @ rx @ rz


def run(path, vp=(4304, 1800), verbose=False):
    import numpy as np
    head, draws = capparse.parse(path)
    g = [d for d in draws if d['targets'] == 7 and d['vp'] == vp and capparse.model_view(d)]
    res = {}
    for name, wheels in (('truck', TRUCK), ('trailer', TRAILER)):
        f = find_frame(g, wheels)
        if not f:
            continue
        m = f[1]
        inside = []
        for d in g:
            q = tuple(r[3] for r in capparse.model_view(d))
            lp = inv_apply(m, q)
            if all(BOX[name][i][0] <= lp[i] <= BOX[name][i][1] for i in range(3)):
                inside.append((d, lp))
        res[name] = (f[0], m, inside)
    line = '%s %s %5.1f km/h tr=%d draws=%4d' % (path[-26:], head['time'], head['speed'], head.get('trailer_connected', 0), len(g))
    for name in ('truck', 'trailer'):
        if name in res:
            line += ' | %s wheels %d, %d draws in box' % (name, res[name][0], len(res[name][2]))
    if 'truck' in res and 'trailer' in res:
        mt, ml = np.array(res['truck'][1]), np.array(res['trailer'][1])
        rel_cam = np.linalg.solve(mt[:, :3], ml[:, 3] - mt[:, 3])  # trailer origin in truck frame
        tw, lw = head['truck'], head['trailer']
        rt = rot_world(*tw[3:6])
        rel_tel = rt.T @ (np.array(lw[:3]) - np.array(tw[:3]))
        line += ' | trailer in truck frame: render (%.2f %.2f %.2f) telemetry (%.2f %.2f %.2f)' % (*rel_cam, *rel_tel)
    print(line)
    if verbose:
        for name in res:
            labels = collections.Counter(d['label'][:30] for d, lp in res[name][2])
            print('   ', name, labels.most_common(8))
    return res


if __name__ == '__main__':
    for f in sys.argv[1:]:
        run(f)
