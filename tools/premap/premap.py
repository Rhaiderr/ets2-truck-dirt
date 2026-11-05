"""P1: the pre-map of the vehicles' meshes, from the game's own models (extracted with the extractor into probe/).

Each .pmg piece gives a line: its index count, its vertex count, the box of its vertices (model space, the header's,
verified equal to the vertices' own on the VNL 2025) and what it is:
  int_ext  the interior seen from outside (an interior accessory's ext_model: /interior/external/, and N58 every
           ext_model the game's definitions name, as the Peterbilt 389's /upgrade/ext_interior/peterbilt_389/)
  int      the interior camera's own model (/interior/model/, /interior/anim/)
  cargo    a load seen on a trailer (a .../cargo/ folder)
  ext      everything else of the player's vehicles
N58c: for each variant of a model (its .pmd, next to the .pmg) whose visible parts have 2+ pieces of one material, an
"m" line too:
those pieces as one (their counts and vertices summed, the size of their joint box). The game draws them as one mesh,
its vertices already at the model's place [verified: ATS 1.62 05/11, the Peterbilt 389's interior seen from outside,
upgrade/ext_interior/peterbilt_389/stock.pmg, variant 1 = parts 1, 3, 4 = one draw 4989/1643 of the size of pieces
1, 3, 4 together, 0.84 m up and 1.07 m back]: the plugin matches an "m" line by its size only.
Traffic (/vehicle/ai, /vehicle/car) is left out, and of /vehicle/props and /vehicle/machine only their cargo.
The plugin reads a truck mesh once (index and vertex buffers) and looks its count, vertex count and box up here.

  python tools/premap/premap.py <extracted root> <out.premap> [name]
  python tools/premap/premap.py --all <root, one folder per archive (extractor -a -S)> <out dir> <prefix> [defs]

defs (N58): a folder with the game's /def/vehicle definitions (extractor -a -S "-p=/def/vehicle" "-f=*.sii"); each
ext_model it names is int_ext wherever its folder is.

Only numbers leave the game's files: no geometry, no texture.
"""
import os
import re
import struct
import sys

EXT_MODELS = set()  # N58: the ext_model paths of the game's definitions, as /vehicle/....pmg


def read_defs(root):
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith('.sii'):
                with open(os.path.join(d, f), encoding='utf-8', errors='replace') as h:
                    for m in re.finditer(r'ext_model\s*:\s*"([^"]+)"', h.read()):
                        EXT_MODELS.add(os.path.splitext(m.group(1))[0] + '.pmg')

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
    if '/interior/external/' in r or r in EXT_MODELS:
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
        material = struct.unpack_from('<I', b, o + 16)[0]  # N58c: [verified: 0-11 on the 389's truck.pmg, 12 materials]
        out.append((count, verts, box, material))
    return out


PART = 24  # N58c: a PMG 0x15 part: token (8), piece count, first piece, locator count, first locator
VISIBLE = 46933326286  # the token of the PMD attribute "visible"


def parts(path):
    """N58c: each part's pieces (first, count) of a PMG 0x15, or None."""
    b = open(path, 'rb').read()
    n = struct.unpack_from('<I', b, 8)[0]
    at = struct.unpack_from('<6I', b, 0x48)[0]
    if at + PART * n > len(b):
        return None
    out = [struct.unpack_from('<2I', b, at + PART * i + 8)[::-1] for i in range(n)]
    # Each part's pieces inside the model's (a model whose parts say otherwise is left as single pieces).
    pieces_n = struct.unpack_from('<I', b, 4)[0]
    return out if all(first + count <= pieces_n for first, count in out) else None


def variants(path, part_count):
    """N58c: each variant's visible parts from a PMD 4 (the descriptor next to the PMG), or None."""
    try:
        b = open(path, 'rb').read()
    except OSError:
        return None
    if len(b) < 64:
        return None
    h = struct.unpack_from('<16I', b, 0)
    (version, _materials, _looks, _, nvar, nparts, nattr, vsize, _, _, _, parts_at, values_at, attr_at) = h[:14]
    if version != 4 or nparts != part_count or values_at + nvar * vsize > len(b) or attr_at + 16 * nattr > len(b):
        return None
    attrs = [struct.unpack_from('<QII', b, attr_at + 16 * i) for i in range(nattr)]
    out = []
    for v in range(nvar):
        seen = []
        for p in range(nparts):
            first, last = struct.unpack_from('<2I', b, parts_at + 8 * p)
            vis = True  # a part without a "visible" attribute is always drawn
            for a in range(first, min(last, nattr)):
                if attrs[a][0] == VISIBLE:
                    vis = struct.unpack_from('<I', b, values_at + v * vsize + attrs[a][2])[0] != 0
            if vis:
                seen.append(p)
        out.append(seen)
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
            for i, (count, verts, box, _) in enumerate(ps):
                lines.append('p %d %d %s %s %s#%d' % (count, verts, ' '.join('%.4f' % x for x in box), k, rel, i))
            # N58c: each variant's visible pieces of one material as one mesh.
            pp = parts(path)
            vs = variants(path[:-4] + '.pmd', len(pp)) if pp else None
            done = set()
            for v, seen in enumerate(vs or []):
                vis = sorted(i for prt in seen for i in range(pp[prt][0], pp[prt][0] + pp[prt][1]) if i < len(ps))
                for mat in sorted(set(ps[i][3] for i in vis)):
                    ids = [i for i in vis if ps[i][3] == mat]
                    if len(ids) < 2 or tuple(ids) in done:
                        continue
                    done.add(tuple(ids))
                    count = sum(ps[i][0] for i in ids)
                    verts = sum(ps[i][1] for i in ids)
                    lo = [min(ps[i][2][a] for i in ids) for a in range(3)]
                    hi = [max(ps[i][2][a + 3] for i in ids) for a in range(3)]
                    lines.append('m %d %d %s %s %s@%d' % (count, verts, ' '.join('%.4f' % x for x in lo + hi), k, rel, v))
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
        if len(sys.argv) > 5:
            read_defs(sys.argv[5])
            print('defs: %d ext_model paths' % len(EXT_MODELS))
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
