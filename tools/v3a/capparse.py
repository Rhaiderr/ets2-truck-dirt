"""Parser for V3a frame captures (captures/capture_NNN.txt)."""
import re

DRAW = re.compile(r'^d (\d+) (\w+) n=(\d+) x(\d+) .*?\| ps (\S+) (.*?) \| vs (\S+) \| rt (\d+)(?::(\S+))? (\d+)x(\d+) \| ds (-?\d+) \| vp (\d+)x(\d+)@(-?\d+),(-?\d+)(.*)$')


def parse(path):
    head = {}
    draws = []
    cur = None
    cb = None
    rows_by_draw = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('# '):
            m = re.match(r'# (truck|trailer\.0 connected (\d)) placement (\S+) (\S+) (\S+) h (\S+) p (\S+) r (\S+)', line)
            if m:
                key = 'truck' if m.group(1) == 'truck' else 'trailer'
                head[key] = [float(x) for x in m.groups()[2:]]
                if key == 'trailer':
                    head['trailer_connected'] = m.group(2) == '1'
            m = re.match(r'# capture (\d+) at (\S+): (\d+) draws.*?, ([-\d.]+) km/h', line)
            if m:
                head['number'], head['time'], head['speed'] = int(m.group(1)), m.group(2), float(m.group(4))
            m = re.match(r'# (truck|trailer\.0) wheel (\d+) rotation (\S+) steering (\S+) deflection (\S+)', line)
            if m:
                head.setdefault(m.group(1) + '_wheels', []).append([float(x) for x in m.groups()[2:]])
            continue
        m = DRAW.match(line)
        if m:
            g = m.groups()
            rest = g[16]
            st = re.search(r'stencil (\d) ref (\d+) r(\w+) w(\w+) f(\d+) p(\d+)', rest)
            dep = re.search(r'depth (\d)/(\d)/f(\d+)', rest)
            cur = dict(i=int(g[0]), kind=g[1], n=int(g[2]), inst=int(g[3]), ps=g[4], label=g[5], vs=g[6],
                       targets=int(g[7]), fmts=g[8] or '', ds=int(g[11]), vp=(int(g[12]), int(g[13])),
                       stencil=(int(st.group(1)), int(st.group(2)), st.group(3), st.group(4), int(st.group(5)), int(st.group(6))) if st else None,
                       depth=(int(dep.group(1)), int(dep.group(2)), int(dep.group(3))) if dep else None,
                       tex=re.search(r'tex (\S+) (\d+)x(\d+) f(\d+)', rest).groups() if 'tex ' in rest else None,
                       vb=re.search(r'vb (\S+)/', rest).group(1) if 'vb ' in rest else None,
                       cb={})
            draws.append(cur)
            cb = None
            continue
        m = re.match(r'  vs_cb(\d) (\S+) first (\d+) num (\d+) width (\d+)(?: dyn)?(?: = same rows as d (\d+))?', line)
        if m and cur is not None:
            slot = int(m.group(1))
            if m.group(6):
                ref = draws[int(m.group(6))]['cb'].get(('rows', m.group(2)))
                cur['cb'][slot] = ref
            else:
                cur['cb'][slot] = []
                cb = cur['cb'][slot]
                cur['cb'][('rows', m.group(2))] = cb
            continue
        m = re.match(r'    r(\d+) (\S+) (\S+) (\S+) (\S+)', line)
        if m and cb is not None:
            cb.append([float(x) for x in m.groups()[1:]])
    # fix "same rows" references that point at the per-buffer key
    for d in draws:
        for k in list(d['cb'].keys()):
            if isinstance(k, tuple):
                del d['cb'][k]
    return head, draws


def _rotation(rows):
    return all(0.25 < sum(x * x for x in r[:3]) < 4.0 for r in rows)


def _matrix_at(rows, k):
    """N29: rows k..k+2 a rotation (rows at right angles), then (0 0 0 1), as tint.cpp's MatrixAt."""
    if len(rows) < k + 4 or not _rotation(rows[k:k + 3]):
        return False
    for a in range(3):
        for b in range(a + 1, 3):
            if abs(sum(rows[k + a][j] * rows[k + b][j] for j in range(3))) >= 0.05:
                return False
    w = rows[k + 3]
    return abs(w[0]) < 1e-4 and abs(w[1]) < 1e-4 and abs(w[2]) < 1e-4 and abs(w[3] - 1) < 1e-4


def model_view(d):
    """3x4 model->camera matrix (VS cb0 rows 0-2; N29: rows 1-3 down when the shader's own constants come first,
    the ATS truck paint) or None."""
    rows = d['cb'].get(0)
    if not rows or len(rows) < 3:
        return None
    if _rotation(rows[:3]):
        return [rows[0], rows[1], rows[2]]
    for k in (1, 2, 3):
        if _matrix_at(rows, k):
            return [rows[k], rows[k + 1], rows[k + 2]]
    return [rows[0], rows[1], rows[2]]
