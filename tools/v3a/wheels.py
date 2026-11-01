"""For each distinct model->camera matrix of the main G-buffer pass, count the
draws sitting at the truck's / trailer's wheel positions (from the config) seen
from that matrix. A frame with >= 3 wheel hits is a candidate vehicle frame."""
import sys, math, collections
import capparse

TRUCK = [(-1.04, 0.501832, -1.79247), (1.04, 0.501832, -1.79247), (-0.9317, 0.501832, 1.41812),
         (0.9317, 0.501832, 1.41812), (-0.9317, 0.501832, 2.76783), (0.9317, 0.501832, 2.76783)]
TRAILER = [(-0.82, 0.512, 1.54165), (0.82, 0.512, 1.54165), (-0.82, 0.512, 2.85739), (0.82, 0.512, 2.85739),
           (-0.82, 0.512, 4.16141), (0.82, 0.512, 4.16141)]


def apply(m, p):
    return tuple(m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3))


def run(path, vp=(4304, 1800), tol=0.08):
    head, draws = capparse.parse(path)
    g = [d for d in draws if d['targets'] == 7 and d['vp'] == vp and capparse.model_view(d)]
    mats = collections.OrderedDict()
    for d in g:
        m = capparse.model_view(d)
        key = tuple(round(v, 3) for row in m for v in row)
        mats.setdefault(key, []).append(d)
    pos = [(tuple(r[3] for r in capparse.model_view(d)), d) for d in g]
    out = []
    for key, ds in mats.items():
        m = capparse.model_view(ds[0])
        for name, wheels in (('truck', TRUCK), ('trailer', TRAILER)):
            hits = []
            for wi, w in enumerate(wheels):
                p = apply(m, w)
                near = [d for (q, d) in pos if math.dist(p, q) < tol]
                if near:
                    hits.append((wi, len(near)))
            if len(hits) >= 3:
                t = [r[3] for r in m]
                out.append((name, len(hits), len(ds), t, hits, ds[0]['i'], ds[0]['label'][:40]))
    print('%s %s %.0f km/h trailer=%s draws=%d frames=%d' % (path[-28:], head['time'], head['speed'],
          head.get('trailer_connected'), len(g), len(mats)))
    for o in sorted(out, key=lambda o: -o[1]):
        print('  %-7s wheels %d  draws@frame %3d  t=(%.2f %.2f %.2f)  hits %s  first d %d %s' % (
            o[0], o[1], o[2], o[3][0], o[3][1], o[3][2], o[4], o[5], o[6]))


if __name__ == '__main__':
    for f in sys.argv[1:]:
        run(f)
