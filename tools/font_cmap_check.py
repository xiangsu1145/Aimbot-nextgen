import struct, sys

def parse_cmap(path):
    data = open(path, 'rb').read()
    num_tables = struct.unpack('>H', data[4:6])[0]
    cmap_off = None
    for i in range(num_tables):
        off = 12 + i * 16
        tag = data[off:off+4]
        if tag == b'cmap':
            cmap_off = struct.unpack('>I', data[off+8:off+12])[0]
    if cmap_off is None:
        raise SystemExit('no cmap')
    n = struct.unpack('>H', data[cmap_off+2:cmap_off+4])[0]
    best = None
    for i in range(n):
        rec = cmap_off + 4 + i * 8
        pid, eid, sub = struct.unpack('>HHI', data[rec:rec+8])
        fmt = struct.unpack('>H', data[cmap_off+sub:cmap_off+sub+2])[0]
        # prefer full unicode (format 12) then BMP (format 4)
        score = {(3,10):3, (0,4):3, (0,3):3, (3,1):2, (0,0):1}.get((pid,eid), 1)
        if fmt == 12: score += 5
        if best is None or score > best[0]:
            best = (score, cmap_off+sub, fmt)
    _, off, fmt = best
    cmap = {}
    if fmt == 4:
        segX2 = struct.unpack('>H', data[off+6:off+8])[0]
        seg = segX2 // 2
        end = [struct.unpack('>H', data[off+14+i*2:off+16+i*2])[0] for i in range(seg)]
        start = [struct.unpack('>H', data[off+16+segX2+i*2:off+18+segX2+i*2])[0] for i in range(seg)]
        delta = [struct.unpack('>h', data[off+16+2*segX2+i*2:off+18+2*segX2+i*2])[0] for i in range(seg)]
        ro_base = off+16+3*segX2
        ro = [struct.unpack('>H', data[ro_base+i*2:ro_base+i*2+2])[0] for i in range(seg)]
        for i in range(seg):
            for c in range(start[i], min(end[i], 0xFFFF)+1):
                if ro[i] == 0:
                    g = (c + delta[i]) & 0xFFFF
                else:
                    idx = ro_base + i*2 + ro[i] + (c - start[i])*2
                    g = struct.unpack('>H', data[idx:idx+2])[0]
                    if g: g = (g + delta[i]) & 0xFFFF
                if g:
                    cmap[c] = g
    elif fmt == 12:
        ngroups = struct.unpack('>I', data[off+12:off+16])[0]
        for i in range(ngroups):
            s, e, gid = struct.unpack('>III', data[off+16+i*12:off+28+i*12])
            for c in range(s, e+1):
                cmap[c] = gid + (c - s)
    return cmap

for path, name in [(sys.argv[1], 'font1'), (sys.argv[2], 'font2')]:
    cmap = parse_cmap(path)
    print(f'--- {path}')
    print(f'    glyphs mapped: {len(cmap)}  (max cp U+{max(cmap):04X})')
    tests = {
        'Basic Latin A': 0x41, 'Basic Latin z': 0x7A, 'Latin-1 é': 0xE9,
        'CJK 中': 0x4E2D, 'CJK 文': 0x6587, 'CJK 瞄': 0x7784, 'CJK 准': 0x51C6,
        'CJK 设': 0x8BBE, 'CJK 置': 0x7F6E, 'CJK 停': 0x505C, 'CJK 止': 0x6B62,
        'CJK 启': 0x542F, 'CJK 动': 0x52A8, 'CJK 服': 0x670D, 'CJK 务': 0x52A1,
        'Fullwidth ，': 0xFF0C, 'Fullwidth ：': 0xFF1A,
    }
    for label, cp in tests.items():
        print(f'      {label:18s} U+{cp:04X}  {"YES" if cp in cmap else "no "}')
