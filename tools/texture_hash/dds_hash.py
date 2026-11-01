"""Per-mip FNV-1a hashes of a DDS file, the way the game uploads it.

Same method as tools/tirecat of the tire plugin: the data of each mip level
is hashed as one block (rows of 4x4 blocks for BC formats, rows of pixels
otherwise). The plugin hashes what the game sends to UpdateSubresource(1)
row by row, skipping the row pitch padding, so the hashes match.

Usage: python -I dds_hash.py <file.dds>...
"""
import struct
import sys

FNV_SEED = 1469598103934665603
FNV_PRIME = 1099511628211

# DXGI format -> (bytes per 4x4 block, bytes per pixel); one of them is 0.
FORMATS = {
    71: (8, 0), 72: (8, 0), 80: (8, 0), 81: (8, 0),            # BC1, BC4
    74: (16, 0), 75: (16, 0), 77: (16, 0), 78: (16, 0),        # BC2, BC3
    83: (16, 0), 84: (16, 0), 98: (16, 0), 99: (16, 0),        # BC5, BC7
    28: (0, 4), 29: (0, 4), 87: (0, 4), 91: (0, 4),            # RGBA8 / BGRA8
    61: (0, 1), 49: (0, 2), 56: (0, 2),                        # R8, R8G8, R16
}
LEGACY = {b'DXT1': 71, b'DXT3': 74, b'DXT5': 77, b'ATI1': 80, b'BC4U': 80, b'ATI2': 83, b'BC5U': 83}


def fnv1a(data):
    h = FNV_SEED
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def legacy_format(flags, fourcc, bits, masks):
    if flags & 0x4:
        return LEGACY.get(fourcc, 0)
    if bits == 32:
        return 87 if masks[0] == 0x00FF0000 else 28
    if bits == 8:
        return 61
    if bits == 16:
        return 49
    return 0


def describe(path):
    d = open(path, 'rb').read()
    if d[:4] != b'DDS ':
        return path + ': not a DDS'
    h, w, mips = struct.unpack_from('<3I', d, 12)[0], struct.unpack_from('<I', d, 16)[0], struct.unpack_from('<I', d, 28)[0]
    pf_flags, fourcc, bits = struct.unpack_from('<I', d, 80)[0], d[84:88], struct.unpack_from('<I', d, 88)[0]
    masks = struct.unpack_from('<4I', d, 92)
    off = 128
    if fourcc == b'DX10':
        fmt = struct.unpack_from('<I', d, 128)[0]
        off += 20
    else:
        fmt = legacy_format(pf_flags, fourcc, bits, masks)
    mips = mips or 1
    bb, pb = FORMATS.get(fmt, (0, 0))
    out = ['%s: %dx%d format %d mips %d' % (path, w, h, fmt, mips)]
    if not bb and not pb:
        return out[0] + ' (unhandled format)'
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        size = ((mw + 3) // 4) * ((mh + 3) // 4) * bb if bb else mw * mh * pb
        if off + size > len(d):
            out.append('  mip %d truncated' % m)
            break
        out.append('  mip %d %dx%d %d bytes hash %016x' % (m, mw, mh, size, fnv1a(d[off:off + size])))
        off += size
    return '\n'.join(out)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(describe(p))
