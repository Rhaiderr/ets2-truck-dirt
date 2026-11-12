# -*- coding: utf-8 -*-
"""The FPS the plugin's own stats lines already measure.

Every `[probe] census_s` seconds the plugin writes `dust: frames N, ...` to gpu_probe.log.
Between two such lines the frames and the clock give the frame rate of that stretch, and the
other counters say what the plugin was doing while the frames were being drawn:

  passes/f   the dust pass per frame: 1 in the exterior camera, one more per view the game
             draws the rig in (the mirrors), so this is the plugin's own work multiplied
  gbuf/f     G-buffer draws per frame: how heavy the scene is (the game's own cost)
  unit/f     draws of the rig's own classes
  rt3/f      full copies of the paint reflection (N25), one per pass

Usage: python tools/fps_from_log.py "<.../TruckDirt/gpu_probe.log>"
"""
import re
import sys

PAT = {'passes': r'passes (\d+)', 'unit': r'unit draws (\d+)', 'gbuf': r'g-buffer draws (\d+)',
       'rt3': r'rt3 copies (\d+)', 'interior': r'interior (\d+)', 'marked': r'marked (\d+)',
       'glass': r'glass dirt \(N40\): (\d+) draw'}


def rows(path):
    out = []
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'(\d\d):(\d\d):(\d\d)\.(\d+) dust: frames (\d+)', ln)
        if not m:
            continue
        t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000
        r = {'t': t, 'f': int(m.group(5))}
        for k, p in PAT.items():
            g = re.search(p, ln)
            r[k] = int(g.group(1)) if g else 0
        out.append(r)
    return out


def main(path):
    rs = rows(path)
    if len(rs) < 2:
        print('no stats lines in', path, '(is [probe] census_s set?)')
        return
    print('time        fps  passes/f  unit/f   gbuf/f   rt3/f  interior/f  marked/f  glass/f')
    for a, b in zip(rs, rs[1:]):
        dt, df = b['t'] - a['t'], b['f'] - a['f']
        if dt <= 0 or df <= 0:
            continue
        per = lambda k: (b[k] - a[k]) / df
        h, mi, s = int(b['t'] // 3600), int(b['t'] % 3600 // 60), int(b['t'] % 60)
        tag = '' if per('gbuf') else '   (menu/loading: the pass never ran)'
        print(f'{h:02d}:{mi:02d}:{s:02d} {df / dt:7.1f} {per("passes"):9.2f} {per("unit"):7.1f} '
              f'{per("gbuf"):8.0f} {per("rt3"):7.2f} {per("interior"):11.1f} {per("marked"):9.0f} '
              f'{per("glass"):8.2f}{tag}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'gpu_probe.log')
