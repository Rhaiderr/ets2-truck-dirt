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
import math
import os
import re
import struct
import sys

EXT_MODELS = set()  # N58: the ext_model paths of the game's definitions, as /vehicle/....pmg
CABINS = {}  # N68: {truck id: {(wiper model .pmg, wiper anim .pma)}}
GLASS = {}   # N68: {truck id: ([blade lengths], seconds a stroke takes)}


def read_defs(root):
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith('.sii'):
                with open(os.path.join(d, f), encoding='utf-8', errors='replace') as h:
                    for m in re.finditer(r'ext_model\s*:\s*"([^"]+)"', h.read()):
                        EXT_MODELS.add(os.path.splitext(m.group(1))[0] + '.pmg')
    CABINS.update(read_cabins(root))  # N68: the wipers each truck's cabins use
    GLASS.update(read_glass(root))

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


# ---------------------------------------------------------------------------
# N68: the wipers, from the game's own files.
#
# The user, 06/11: "limpar o para-brisa precisa ser generico para qualquer caminhao... se for preciso podemos
# fazer engenharia reversa no ats e ets2 para descobrir como ele calcula o giro e o tamanho do limpador".
#
# It is all there, and already in the TRUCK's own frame -- the same one the plugin measures the windscreen in:
#   def/vehicle/truck/<id>/cabin/*.sii   accessory_cabin_data: wiper_model (.pmd) and wiper_anim (.pma)
#   <model>.pmg                          the skeleton: wiper_*1 the pivots, wiper_*2 the blades' centres
#   <anim>.pma                           how far each arm turns, and in how long
#   def/vehicle/truck/<id>/interior/glass*.sui   wiper_data: the blade's length (m)
#   def/vehicle/truck/<id>/interior/animations*.sui   wiper_duration_and_delay: the real timing
#
# [verified: Volvo FH 2024, ETS2 1.62, 06/11] pivots (-0.817, 2.119, -3.104) and (0.155, 2.119, -3.133), arms
# 0.557 and 0.883 m, blades 0.70 m, sweeps 87.9 and 103.2 degrees -- and the plugin measures that windscreen at
# z -3.10 with its base at y 2.20, so the pivots land exactly on it. The two arms are NOT symmetric, which is
# why no "pivot and its mirror" model could ever have fitted this truck.
#
# Formats in the development notes on the engine's glass, section 5.

TOKEN_CH = '\x000123456789abcdefghijklmnopqrstuvwxyz_'
BONE = 200       # bytes per PMG 0x15 bone
LOCATOR = 44     # bytes per locator
WIPER_SWEEP_MIN = 0.17  # rad (~10 deg): under this the bone is not an arm (the blade bones hardly turn)


def untoken(v):
    """The SCS token: base 38, 1..10 = 0..9, 11..36 = a..z, 37 = _."""
    s = ''
    while v:
        c = v % 38
        if c >= len(TOKEN_CH):
            return ''
        s += TOKEN_CH[c]
        v //= 38
    return s


def pmg_bones(path):
    """Each bone of a PMG 0x15: name, its rest transform in the MODEL's frame (3x4, row-major) and its parent."""
    try:
        b = open(path, 'rb').read()
    except OSError:
        return []
    if len(b) < 0x60 or b[0] != PMG_VERSION or b[1:4] != b'gmP':
        return []
    n = struct.unpack_from('<I', b, 0x0c)[0]
    at = struct.unpack_from('<I', b, 0x48)[0]
    if not n or at + BONE * n > len(b):
        return []
    out = []
    for i in range(n):
        o = at + BONE * i
        name = untoken(struct.unpack_from('<Q', b, o)[0])
        m = struct.unpack_from('<16f', b, o + 8)  # the absolute rest transform, translation in the 4th column
        parent = struct.unpack_from('<i', b, o + 196)[0]
        out.append((name, m[:12], parent))
    return out


def pma_sweeps(path, bone_count):
    """Each animated bone of a PMA 5: the axis it turns about (the bone's own frame) and how far, in radians,
    plus how long the whole animation is. Returns {pmg bone index: (axis, angle)}, seconds."""
    try:
        a = open(path, 'rb').read()
    except OSError:
        return {}, 0.0
    if len(a) < 0x54 or struct.unpack_from('<I', a, 0)[0] != 5:
        return {}, 0.0
    frames, nb = struct.unpack_from('<2I', a, 4)
    offs = struct.unpack_from('<5I', a, 0x40)
    if not frames or not nb or offs[2] >= len(a):
        return {}, 0.0
    times = struct.unpack_from('<%df' % frames, a, offs[0])
    idx = list(a[offs[1]:offs[1] + nb]) if offs[1] + nb <= offs[2] else list(range(1, nb + 1))
    size = (len(a) - offs[2]) // (nb * frames)
    if size < 32:
        return {}, 0.0
    out = {}
    for i in range(nb):
        qs = [struct.unpack_from('<4f', a, offs[2] + size * (f * nb + i) + 16) for f in range(frames)]
        best, axis, ang = None, (0.0, 0.0, 0.0), 0.0
        for q in qs:
            # q relative to the first frame (q * conj(q0)), the rest pose being frame 0
            pw, px, py, pz = qs[0]
            qw, qx, qy, qz = q
            r = (qw * pw + qx * px + qy * py + qz * pz, qx * pw - qw * px + qy * pz - qz * py,
                 qy * pw - qw * py + qz * px - qx * pz, qz * pw - qw * pz + qx * py - qy * px)
            c = max(-1.0, min(1.0, abs(r[0])))
            a2 = 2.0 * math.acos(c)
            if a2 > ang:
                s = math.sqrt(max(0.0, 1.0 - r[0] * r[0]))
                if s > 1e-6:
                    ang, axis = a2, (r[1] / s, r[2] / s, r[3] / s)
        if ang > WIPER_SWEEP_MIN and i < len(idx) and idx[i] < bone_count:
            out[idx[i]] = (axis, ang)
    return out, sum(times)


def read_glass(def_root):
    """What each truck declares: {truck id: ([blade lengths], [(stroke s, pause s) per speed])}.

    N68c: the three speeds, in the order the game lists them -- the player cycles through these and the SDK
    never says which one is running, so all three are kept."""
    out = {}
    for d, _, fs in os.walk(def_root):
        rel = d.replace(os.sep, '/')
        m = re.search(r'/def/vehicle/truck/([^/]+)/', rel + '/')
        if not m:
            continue
        tid = m.group(1)
        for f in fs:
            if not f.endswith('.sui'):
                continue
            try:
                text = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            lens, speeds = out.get(tid, ([], []))
            for g in re.finditer(r'wiper_data\s*:[^{]*\{([^}]*)\}', text):
                v = re.search(r'length\s*:\s*([0-9.]+)', g.group(1))
                if v:
                    lens.append(float(v.group(1)))
            for g in re.finditer(r'wiper_duration_and_delay\[\]\s*:\s*\(\s*([0-9.]+)\s*,\s*([0-9.]+)', text):
                sp = (float(g.group(1)), float(g.group(2)))
                if sp not in speeds:  # the same table is repeated in the UK variant of the file
                    speeds.append(sp)
            out[tid] = (lens, speeds)
    return out


def read_cabins(def_root):
    """Which wiper model and animation each truck's cabins use: {truck id: {(model .pmg, anim .pma)}}."""
    out = {}
    for d, _, fs in os.walk(def_root):
        rel = (d.replace(os.sep, '/') + '/')
        m = re.search(r'/def/vehicle/truck/([^/]+)/', rel)
        if not m:
            continue
        tid = m.group(1)
        for f in fs:
            if not f.endswith('.sii') and not f.endswith('.sui'):
                continue
            try:
                text = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            for g in re.finditer(r'accessory_cabin_data\s*:[^{]*\{(.*?)\n\}', text, re.S):
                body = g.group(1)
                mm = re.search(r'wiper_model\s*:\s*"([^"]+)"', body)
                aa = re.search(r'wiper_anim\s*:\s*"([^"]+)"', body)
                if mm and aa:
                    out.setdefault(tid, set()).add((os.path.splitext(mm.group(1))[0] + '.pmg', aa.group(1)))
    return out


def wiper_lines(root, cabins, glass):
    """The blade lines for the models that live under this archive's root."""
    lines = []
    for tid in sorted(cabins):
        lens, speeds = glass.get(tid, ([], []))
        for model, anim in sorted(cabins[tid]):
            mp = os.path.join(root, model.lstrip('/').replace('/', os.sep))
            ap = os.path.join(root, anim.lstrip('/').replace('/', os.sep))
            if not os.path.isfile(mp) or not os.path.isfile(ap):
                continue
            bones = pmg_bones(mp)
            if not bones:
                continue
            sweeps, anim_s = pma_sweeps(ap, len(bones))
            if not sweeps:
                continue
            blade = lens[0] if lens else 0.0
            # N68c: every speed the player can pick, "stroke:pause" seconds, in the game's own order.
            rhythm = ','.join('%.2f:%.2f' % s for s in speeds) if speeds else '%.2f:0.00' % anim_s
            n = 0
            for bi in sorted(sweeps):
                # N68b: an arm hangs off the root; a blade hangs off an arm and only counter-rotates a little.
                # The skeleton says which is which, so no name has to be guessed at.
                if bones[bi][2] in sweeps:
                    continue
                axis, ang = sweeps[bi]
                name, m, _ = bones[bi]
                px, py, pz = m[3], m[7], m[11]
                # the axis into the truck's frame
                ax = m[0] * axis[0] + m[1] * axis[1] + m[2] * axis[2]
                ay = m[4] * axis[0] + m[5] * axis[1] + m[6] * axis[2]
                az = m[8] * axis[0] + m[9] * axis[1] + m[10] * axis[2]
                # the arm at rest: this bone's child, which is the blade's centre, minus the pivot
                arm, tip = None, -1
                for ci, (_, cm, cpar) in enumerate(bones):
                    if cpar == bi and ci != bi:
                        arm, tip = (cm[3] - px, cm[7] - py, cm[11] - pz), ci
                        break
                if arm is None:
                    arm = (m[0], m[4], m[8])  # the bone's own x: the arm runs along it
                # N68c: the bone numbers too, so the plugin can read where the arm IS from the model's own
                # bone palette instead of replaying a stroke it cannot time.
                lines.append('b %s %d %d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.4f %s %s' %
                             (tid, n, bi, tip, px, py, pz, ax, ay, az, arm[0], arm[1], arm[2],
                              blade, ang, rhythm, model))
                n += 1
    return lines


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
    lines += wiper_lines(root, CABINS, GLASS)  # N68
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
            print('defs: %d ext_model paths, %d trucks with cabins, %d with glass' %
              (len(EXT_MODELS), len(CABINS), len(GLASS)))
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
