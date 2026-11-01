"""Checks the SDK cabin offset against the cab's draws: in a capture, the
truck's frame comes from its wheels; draws whose rotation in that frame is a
small tilt (the cab on its suspension) are compared with the telemetry's
cabin offset (heading/pitch/roll in turns, about the cabin pivot).

N15: this script's reading led to the transposed cab rotation the tracker used until the D4d; the
cab turns by the offset's rotation as it is. The check that counts is rigtest's cab check (plain
run), which compares the tracker's own cab frame with the cab's draws in every capture."""
import sys, math, collections
import capparse
from wheels import TRUCK, apply

PIVOT = (0.0, 1.24613, -2.32305)  # cabin.position of the V3a truck [verified: configs.log]


def rel_rot(a, b):
    return [[sum(a[k][i] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def inv_apply(m, p):
    d = [p[i] - m[i][3] for i in range(3)]
    return tuple(m[0][c] * d[0] + m[1][c] * d[1] + m[2][c] * d[2] for c in range(3))


def euler(h, p, r):
    """SCS: heading about +Y, then pitch about X, then roll about Z (turns)."""
    a, b, c = (x * 2 * math.pi for x in (h, p, r))
    ry = [[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]]
    rx = [[1, 0, 0], [0, math.cos(b), -math.sin(b)], [0, math.sin(b), math.cos(b)]]
    rz = [[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]]
    mul = lambda x, y: [[sum(x[i][k] * y[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mul(mul(ry, rx), rz)


def run(path, vp=(4304, 1800)):
    head, draws = capparse.parse(path)
    cab = None
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# cabin offset'):
            v = line.split()
            cab = [float(v[3]), float(v[4]), float(v[5]), float(v[7]), float(v[9]), float(v[11])]
            break
    g = [d for d in draws if d['targets'] >= 4 and d['vp'] == vp and capparse.model_view(d)]
    pos = [tuple(r[3] for r in capparse.model_view(d)) for d in g]
    best = None
    for d in g:
        m = capparse.model_view(d)
        hits = sum(1 for w in TRUCK if any(math.dist(apply(m, w), q) < 0.08 for q in pos))
        if hits >= 5 and (best is None or hits > best[0]):
            best = (hits, m)
    if not best or not cab:
        print(path[-28:], 'no frame or no cabin offset')
        return
    f = best[1]
    h = cab[3] - round(cab[3])
    expect = euler(h, cab[4], cab[5])
    print('%s cabin offset pos %s h %.6f p %.6f r %.6f' % (path[-28:], cab[:3], h, cab[4], cab[5]))
    print('   expected rotation rows: %s' % [[round(v, 5) for v in row] for row in expect])
    tilts = collections.Counter()
    for d in g:
        m = capparse.model_view(d)
        r = rel_rot(f, m)
        lp = inv_apply(f, tuple(row[3] for row in m))
        off = max(abs(r[i][j] - (1 if i == j else 0)) for i in range(3) for j in range(3))
        if 0.0005 < off < 0.05 and abs(lp[0]) < 1.4 and -0.5 < lp[1] < 4.5 and -3 < lp[2] < 0:
            tilts[tuple(round(r[i][j], 4) for i in range(3) for j in range(3) if i != j)] += 1
    for k, n in tilts.most_common(3):
        print('   %3d draws tilted, off-diagonal (01 02 10 12 20 21): %s' % (n, k))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p)
