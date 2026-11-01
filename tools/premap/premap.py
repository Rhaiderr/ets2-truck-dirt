"""P1: the pre-map of the vehicles' meshes, from the game's own models (extracted with the extractor into probe/).

Each .pmg piece gives a line: its index count, its vertex count, the box of its vertices (model space, the header's,
verified equal to the vertices' own on the VNL 2025) and what it is:
  int_ext  the interior seen from outside (an interior accessory's ext_model: /interior/external/)
  int      the interior camera's own model (/interior/model/, /interior/anim/)
  cargo    a load seen on a trailer (a .../cargo/ folder)
  ext      everything else of the player's vehicles
Traffic (/vehicle/ai, /vehicle/car) is left out, and of /vehicle/props and /vehicle/machine only their cargo.
The plugin reads a truck mesh once (index and vertex buffers) and looks its count, vertex count and box up here.

  python tools/premap/premap.py <extracted root> <out.premap> [name]
  python tools/premap/premap.py --all <root, one folder per archive (extractor -a -S)> <out dir> <prefix>

Only numbers leave the game's files: no geometry, no texture.
"""
import os
import struct
import sys

PMG_VERSION = 0x15
PIECE = 100  # bytes per piece record (PMG 0x15)


def kind_of(rel):
    r = rel.replace(os.sep, '/')
    if r.startswith('/vehicle/ai/') or r.startswith('/vehicle/car/'):
        return None
    if '/cargo/' in r:
        return 'cargo'
    if r.startswith('/vehicle/props/') or r.startswith('/vehicle/machine/'):
        return None
    if '/interior/external/' in r:
        return 'int_ext'
    if '/interior/model/' in r or '/interior/anim' in r:
        return 'int'
    return 'ext'


def pieces(path):
    b = open(path, 'rb').read()
    if len(b) < 0x60 or b[0] != PMG_VERSION or b[1:4] != b'gmP':
        return None
    n = struct.unpack_from('<I', b, 4)[0]
    at = struct.unpack_from('<6I', b, 0x48)[3]
    out = []
    for i in range(n):
        o = at + PIECE * i
        if o + PIECE > len(b):
            return None
        count, verts = struct.unpack_from('<2I', b, o)
        box = struct.unpack_from('<6f', b, o + 36)  # low corner, high corner
        out.append((count, verts, box))
    return out


def table(root, out, name):
    lines, files, skipped = [], 0, []
    for d, _, fs in os.walk(root):
        for f in sorted(fs):
            if not f.endswith('.pmg'):
                continue
            path = os.path.join(d, f)
            rel = os.path.relpath(path, root).replace(os.sep, '/')
            k = kind_of('/' + rel)
            if k is None:
                continue
            ps = pieces(path)
            if ps is None:
                skipped.append(rel)
                continue
            files += 1
            for i, (count, verts, box) in enumerate(ps):
                lines.append('p %d %d %s %s %s#%d' % (count, verts, ' '.join('%.4f' % x for x in box), k, rel, i))
    if not lines:
        return 0
    with open(out, 'w', encoding='utf-8', newline='\n') as w:
        w.write('truck dirt premap 1\nname %s\n' % name)
        w.write('\n'.join(sorted(lines)) + '\n')
    print('%s: %d models, %d pieces, %d skipped (not PMG 0x15) %s' % (name, files, len(lines), len(skipped), skipped[:3]))
    return len(lines)


def main():
    if sys.argv[1] == '--all':
        root, out_dir, prefix = sys.argv[2], sys.argv[3], sys.argv[4]
        os.makedirs(out_dir, exist_ok=True)
        total = 0
        for archive in sorted(os.listdir(root)):
            if os.path.isdir(os.path.join(root, archive)):
                total += table(os.path.join(root, archive), os.path.join(out_dir, '%s_%s.premap' % (prefix, archive)),
                               '%s %s' % (prefix, archive))
        print('total %d pieces' % total)
        return
    table(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else os.path.basename(os.path.normpath(sys.argv[1])))


if __name__ == '__main__':
    main()
