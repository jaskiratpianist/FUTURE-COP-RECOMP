#!/usr/bin/env python3
"""Future Cop texture pages: list, dump to PNG, and check replacements.

    .venv/bin/python -I tools/fcop_textures.py list  game_files/FCopLAPD/missions/M1A1
    .venv/bin/python -I tools/fcop_textures.py dump  game_files/FCopLAPD/missions/M1A1 build/textures
    .venv/bin/python -I tools/fcop_textures.py region game_files/FCopLAPD/missions/M1A1 10 0 0 256 160 out_dir
        (writes the region of page id 10 as <x>_<y>_<w>_<h>_<hash>.png: the name an addon uses)

Mission files are EA chunk streams: 'CTRL', then 'SHOC' chunks (tags are stored
byte-reversed) holding either a resource header ('SHDR': type such as 'Cbmp', id, size) or a
piece of the resource's data ('SDAT'), plus 'FILL' padding up to sector boundaries. A Cbmp
(texture page) is 256x256 16-bit pixels plus a header. Each page is named by the FNV-1a
hash of its pixel data. An addon replaces a rectangle of a page: addons/<Name>/textures/
<x>_<y>_<w>_<h>_<hash>.png, where hash is the FNV-1a of the original 16-bit pixels of that
rectangle (row by row); the game's addon loader (recomp/platform/addons.c) patches every page
whose rectangle matches. The PNG may be larger than the rectangle (higher resolution).
"""
import os, struct, sys, zlib

PAGE = 256 * 256 * 2


def chunks(data):
    """Resources in file order: (type, id, payload bytes)."""
    o, cur, out = 0, None, []
    while o + 8 <= len(data):
        tag, size = data[o:o + 4], struct.unpack_from('<I', data, o + 4)[0]
        if tag == b'LLIF' or size < 8:  # padding: continue at the next chunk
            n = data.find(b'COHS', o + 4)
            if n < 0:
                break
            o = n
            continue
        if tag == b'COHS':
            sub = data[o + 16:o + 20]
            if sub == b'RDHS':
                rtype = data[o + 24:o + 28][::-1].decode('latin-1')
                rid, rsize = struct.unpack_from('<II', data, o + 28)
                cur = [rtype, rid, rsize, bytearray()]
                out.append(cur)
            elif sub == b'TADS' and cur is not None:
                cur[3] += data[o + 20:o + size]
        o += size
    return [(t, i, bytes(p[:s])) for t, i, s, p in out]


def fnv1a(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h


def page_pixels(payload):
    """The 256x256 pixel block of a Cbmp payload (the end of it; the header comes first)."""
    return payload[-PAGE:] if len(payload) >= PAGE else None


def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def to_rgb(px):
    out = bytearray()
    for (v,) in struct.iter_unpack('<H', px):  # 555
        out += bytes((((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31))
    return bytes(out)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    pages = [(i, p) for t, i, p in chunks(open(path, 'rb').read()) if t == 'Cbmp']
    if cmd == 'list':
        for rid, p in pages:
            px = page_pixels(p)
            print(f'Cbmp id {rid:4d}  {len(p)} bytes  hash {fnv1a(px):08x}' if px else f'Cbmp id {rid} {len(p)} bytes (no page)')
    elif cmd == 'region':
        region_cmd(path, *map(int, sys.argv[3:8]), sys.argv[8])
        return
    elif cmd == 'dump':
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        base = os.path.basename(path)
        for rid, p in pages:
            px = page_pixels(p)
            if px:
                name = f'{base}_{rid}_{fnv1a(px):08x}.png'
                png(os.path.join(out, name), 256, 256, to_rgb(px))
                print(name)


def region_pixels(px, x, y, w, h):
    return b''.join(px[(y + r) * 512 + x * 2:(y + r) * 512 + (x + w) * 2] for r in range(h))


def region_cmd(path, rid, x, y, w, h, out):
    for t, i, p in chunks(open(path, 'rb').read()):
        if t == 'Cbmp' and i == rid:
            rp = region_pixels(page_pixels(p), x, y, w, h)
            os.makedirs(out, exist_ok=True)
            name = os.path.join(out, f'{x}_{y}_{w}_{h}_{fnv1a(rp):08x}.png')
            png(name, w, h, to_rgb(rp))
            print(name)


if __name__ == '__main__':
    main()
