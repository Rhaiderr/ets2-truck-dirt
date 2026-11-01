import sys, zlib, struct, re
data = open(sys.argv[1], 'rb').read()
print('size', len(data), 'header u32:', struct.unpack_from('<8I', data, 4))
for m in (b'#version', b'void main', b'DXBC', b'OSGN', b'SV_Target'):
    i = data.find(m)
    print(m, i)
found = False
for off in range(0, min(len(data), 4096)):
    for wbits in (15, -15):
        try:
            d = zlib.decompressobj(wbits)
            out = d.decompress(data[off:])
            if len(out) > 200:
                print('zlib ok at', off, 'wbits', wbits, 'len', len(out))
                open(sys.argv[2], 'wb').write(out)
                found = True
                break
        except Exception:
            pass
    if found: break
if not found:
    txt = re.findall(rb'[\x20-\x7e]{6,}', data)
    print('ascii runs:', [t[:60] for t in txt[:40]])
