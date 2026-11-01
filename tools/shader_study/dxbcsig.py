import sys, struct
data = open(sys.argv[1], 'rb').read()
base = data.find(b'DXBC')
d = data[base:]
n = struct.unpack_from('<I', d, 28)[0]
offs = struct.unpack_from('<%dI' % n, d, 32)
for o in offs:
    tag = d[o:o+4]
    if tag in (b'ISGN', b'OSGN', b'OSG5', b'ISG1', b'OSG1'):
        c = o + 8
        cnt = struct.unpack_from('<I', d, c)[0]
        print(tag.decode(), cnt)
        for i in range(cnt):
            e = c + 8 + i * 24
            name_off, idx, sv, ctype, reg, mask = struct.unpack_from('<IIIIIB', d, e)
            name = d[c + name_off: d.index(b'\0', c + name_off)].decode()
            print('  ', name, idx, 'reg', reg, 'type', ctype, 'mask', hex(mask))
    else:
        print('chunk', tag)
