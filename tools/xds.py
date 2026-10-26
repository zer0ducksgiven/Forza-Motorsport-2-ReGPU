"""Decodes FM2 UI .xds textures (52-byte header with a Xenos fetch constant, tiled DXT1/3/5, 8-in-16 endian) to RGBA."""
import struct, zipfile
from PIL import Image

def outer(y, width, l):
    macro = ((y >> 5) * (width >> 5)) << (l + 7)
    micro = ((y & 6) << 2) << l
    return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + l)) + ((y & 1) << 4)

def inner(x, y, base, l):
    macro = (x >> 5) << (l + 7)
    micro = (x & 7) << l
    o = base + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    return ((o & ~0x1FF) << 3) + ((o & 0x1C0) << 2) + (o & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)

def c565(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31

def color_block(b, img, px, py, one_bit):
    c0, c1, bits = struct.unpack("<HHI", b)
    a, bb = c565(c0), c565(c1)
    if c0 > c1 or not one_bit:
        cs = [a + (255,), bb + (255,), tuple((2 * a[i] + bb[i]) // 3 for i in range(3)) + (255,), tuple((a[i] + 2 * bb[i]) // 3 for i in range(3)) + (255,)]
    else:
        cs = [a + (255,), bb + (255,), tuple((a[i] + bb[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
    out = []
    for i in range(16):
        out.append(cs[(bits >> (2 * i)) & 3])
    return out

def decode(data):
    hdr, payload = data[:52], data[52:]
    d = struct.unpack(">13I", hdr)
    fmt = d[8] & 0x3F
    size = d[9]
    w, h = (size & 0x1FFF) + 1, ((size >> 13) & 0x1FFF) + 1
    blk = {0x12: 8, 0x13: 16, 0x14: 16}.get(fmt)
    if not blk:
        return None, (fmt, w, h)
    # 8-in-16 endian swap
    p = bytearray(payload)
    p[0::2], p[1::2] = payload[1::2], payload[0::2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pitch = (bw + 31) & ~31
    l = 3 if blk == 8 else 4
    img = Image.new("RGBA", (bw * 4, bh * 4))
    px = img.load()
    for by in range(bh):
        row = outer(by, pitch, l)
        for bx in range(bw):
            off = inner(bx, by, row, l)
            b = bytes(p[off:off + blk]) if off + blk <= len(p) else bytes(blk)
            if blk == 8:
                cols = color_block(b, None, 0, 0, True)
            else:
                cols = color_block(b[8:16], None, 0, 0, False)
                if fmt == 0x14:  # DXT5 alpha
                    a0, a1 = b[0], b[1]
                    ab = int.from_bytes(b[2:8], "little")
                    al = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                    cols = [cols[i][:3] + (al[(ab >> (3 * i)) & 7],) for i in range(16)]
                else:  # DXT3
                    ab = int.from_bytes(b[0:8], "little")
                    cols = [cols[i][:3] + (((ab >> (4 * i)) & 15) * 17,) for i in range(16)]
            for i in range(16):
                px[bx * 4 + i % 4, by * 4 + i // 4] = cols[i]
    return img.crop((0, 0, w, h)), (fmt, w, h)
