"""Export a V3a capture as CSV for tools/rigtest: the G-buffer DrawIndexed
draws with their mesh key and model->camera matrix, the view id (viewport +
run, as the DLL numbers them) and the telemetry pose."""
import re, sys
import capparse
from wheels import TRUCK, TRAILER

TRAILER_HOOK = (0, 1, -5.06578)
CABIN_PIVOT = (0.0, 1.24613, -2.32305)  # cabin.position of the V3a truck [verified: configs.log]
NEXT_HOOK = None  # N24: trailer.1's hook.position; when set, a "next" line with its placement (# trailer.1 ...)
# N21: the trailers behind trailer.0 as units 2.. (trailer.1 ..): (hook.position, wheel positions) each; their
# placement, coupling and deflection come from the capture's "# trailer.N" lines.
MORE = []


def export(path, out):
    head, draws = capparse.parse(path)
    raw = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^d (\d+) \w+ n=\d+ x\d+ start=(\d+) base=(-?\d+) .*\| ib (\S+) f\d+ \+\d+ \| vb (0x[0-9A-Fa-f]+)/', line)
        if m:
            raw[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), m.group(4), m.group(5))
    with open(out, 'w') as f:
        f.write('placement,0,%s\n' % ','.join(str(x) for x in head['truck']))
        f.write('placement,1,%s,%d\n' % (','.join(str(x) for x in head['trailer']), 1))
        f.write('unit,0,%s\n' % ','.join('%g,%g,%g' % w for w in TRUCK))
        f.write('unit,1,%s\n' % ','.join('%g,%g,%g' % w for w in TRAILER))
        f.write('hook,1,%g,%g,%g\n' % TRAILER_HOOK)
        for i, (hook, wheels) in enumerate(MORE):
            name = 'trailer.%d' % (i + 1)
            place, defl = None, {}
            for line in open(path, encoding='utf-8', errors='replace'):
                v = line.split()
                if len(v) >= 14 and v[:3] == ['#', name, 'connected']:
                    place = ([v[5], v[6], v[7], v[9], v[11], v[13]], v[3])
                elif len(v) >= 10 and v[:3] == ['#', name, 'wheel'] and 'deflection' in v:
                    defl[int(v[3])] = v[v.index('deflection') + 1]
            if not place:
                continue
            f.write('placement,%d,%s,%s\n' % (i + 2, ','.join(place[0]), place[1]))
            f.write('unit,%d,%s\n' % (i + 2, ','.join('%g,%g,%g' % w for w in wheels)))
            f.write('hook,%d,%g,%g,%g\n' % ((i + 2,) + tuple(hook)))
            if defl:
                f.write('deflection,%d,%s\n' % (i + 2, ','.join(defl.get(w, '0') for w in range(len(wheels)))))
        if NEXT_HOOK:
            for line in open(path, encoding='utf-8', errors='replace'):
                v = line.split()
                if len(v) >= 14 and v[:3] == ['#', 'trailer.1', 'connected']:
                    place = [v[5], v[6], v[7], v[9], v[11], v[13]]
                    f.write('next,1,%g,%g,%g,%s,%s\n' % (NEXT_HOOK + (','.join(place), v[3])))
                    break
        cab = None
        for line in open(path, encoding='utf-8', errors='replace'):
            if line.startswith('# cabin offset'):
                v = line.split()
                cab = [float(v[3]), float(v[4]), float(v[5]), float(v[7]), float(v[9]), float(v[11])]
                break
        if cab:
            f.write('cab,%g,%g,%g,%s\n' % (CABIN_PIVOT + (','.join('%g' % x for x in cab),)))
        for u, name in ((0, 'truck_wheels'), (1, 'trailer.0_wheels')):
            if head.get(name):
                f.write('deflection,%d,%s\n' % (u, ','.join('%g' % w[2] for w in head[name])))
        views, runs, last = {}, {}, None
        for d in draws:
            g = d['targets'] >= 4 and d['fmts'].startswith('f10,f10,f10,f12')
            if not g:
                last = None
                continue
            vp = d['vp']
            if last != vp:  # a new G-buffer pass of this viewport
                runs[vp] = runs.get(vp, -1) + 1
                last = vp
            key = (vp, runs[vp])
            if key not in views:
                views[key] = len(views)
                f.write('view,%d,%d,%d,%d\n' % (views[key], vp[0], vp[1], runs[vp]))
            m = capparse.model_view(d)
            if d['kind'] != 'DrawIndexed' or not m:
                continue
            start, base, ib, vb = raw[d['i']]
            ref = d['stencil'][1] if d['stencil'] else 0
            f.write('draw,%d,%s,%s,%d,%d,%d,%s,%d\n' % (views[key], vb, ib, start, base, d['n'],
                                                      ','.join('%.7g' % v for row in m for v in row), ref))
    return views


if __name__ == '__main__':
    print(export(sys.argv[1], sys.argv[2]))
