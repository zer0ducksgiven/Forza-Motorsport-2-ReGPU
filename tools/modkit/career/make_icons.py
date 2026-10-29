"""Draws the Random Events category's pictures, a pair of dice in the style of the game's race type icons (white,
thick black outline, soft shadow), and writes them as FM2 UI .xds textures (tiled DXT5, as tools/xds.py reads them)
into career/icons, which random_events.py puts in the mod. Run once when the art changes (needs Pillow); the .xds
files are kept in the repository, so the launcher's Python doesn't need Pillow.

    make_icons.py <Textures.zip>   (game/Media/UI/Textures.zip: the race type icons' headers are reused)
"""
import math, os, struct, sys, zipfile
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..'))
import xds  # tools/xds.py: outer/inner, the tiling

OUT = os.path.join(HERE, 'icons')
PIPS = {1: [(0, 0)], 2: [(-1, -1), (1, 1)], 3: [(-1, -1), (0, 0), (1, 1)], 4: [(-1, -1), (1, -1), (-1, 1), (1, 1)],
        5: [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)], 6: [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)]}


def die(size, face, angle):
    """One die face-on, rotated: white, black outline, black pips (RGBA, transparent around)."""
    s = size * 2
    img = Image.new('RGBA', (s, s))
    d = ImageDraw.Draw(img)
    r, line, half = size * 0.18, size * 0.07, size / 2
    box = (s / 2 - half, s / 2 - half, s / 2 + half, s / 2 + half)
    d.rounded_rectangle(box, r, fill=(255, 255, 255, 255), outline=(0, 0, 0, 255), width=int(line))
    pip = size * 0.085
    for px, py in PIPS[face]:
        cx, cy = s / 2 + px * size * 0.25, s / 2 + py * size * 0.25
        d.ellipse((cx - pip, cy - pip, cx + pip, cy + pip), fill=(0, 0, 0, 255))
    return img.rotate(angle, resample=Image.BICUBIC)


def art(w, h):
    """Two dice, outlined together in black, on a soft shadow; drawn at 4x and scaled down."""
    k = 4
    W, H = w * k, h * k
    size = int(H * 0.52)
    dice = Image.new('RGBA', (W, H))
    for face, angle, cx, cy in ((5, 14, 0.40, 0.52), (3, -18, 0.62, 0.46)):
        d = die(size, face, angle)
        dice.alpha_composite(d, (int(W * cx - d.width / 2), int(H * cy - d.height / 2)))
    alpha = dice.getchannel('A')
    border = alpha.filter(ImageFilter.MaxFilter(int(H * 0.05) | 1))   # the thick black outline round both
    shadow = border.filter(ImageFilter.GaussianBlur(H * 0.03))
    out = Image.new('RGBA', (W, H))
    off = int(H * 0.025)
    out.paste((0, 0, 0, 255), (off, off), shadow.point(lambda a: a * 0.45))
    out.paste((0, 0, 0, 255), (0, 0), border)
    out.alpha_composite(dice)
    return out.resize((w, h), Image.LANCZOS)


GREY, RED = (70, 70, 70), (210, 20, 20)  # the career title bar's strike crosses (roguelike_ui.py's TITLE)


def cross(w, h):
    """An X like the title bar's (its brush font's): a heavy, steep stroke from top left to bottom right, cut flat,
    across a long thin one from bottom left to top right. Its alpha only, drawn at 4x."""
    k = 4
    W, H = w * k, h * k
    img = Image.new('L', (W, H))
    d = ImageDraw.Draw(img)
    for stroke in (((0.23, 0.0), (0.57, 0.03), (0.79, 0.97), (0.43, 1.0)),    # the heavy one
                   ((0.0, 1.0), (0.17, 1.0), (1.0, 0.0), (0.84, 0.0))):      # the thin one
        d.polygon([(u * W, v * H) for u, v in stroke], fill=255)
    return img.resize((w, h), Image.LANCZOS)


def strikes_art(w, h, struck):
    """The car level badge's icon as a car's strikes (Select Car; the port puts the strikes in the garage's car level):
    two crosses, the first red when struck (roguelike_ui.py moves the icon into the middle of the badge's box). Each
    half is its cross's colour throughout (only alpha changes), so no 4x4 block mixes the two."""
    out = Image.new('RGBA', (w, h))
    for i, colour in enumerate((RED if struck else GREY, GREY)):
        half = Image.new('RGBA', (w // 2, h), colour + (0,))
        a = cross(int(w * 0.42), int(h * 0.30))
        mask = Image.new('L', half.size)
        mask.paste(a, ((half.width - a.width) // 2, (h - a.height) // 2))
        half.putalpha(mask)
        out.paste(half, (i * w // 2, 0))
    return out


def dxt5_block(px):
    """16 RGBA pixels -> a DXT5 block (alpha: 8 levels between the block's min and max; colour: endpoints along
    the luminance range, enough for these grey pictures)."""
    a = [p[3] for p in px]
    a0, a1 = max(a), min(a)
    if a0 == a1:
        aidx = [0] * 16
    else:
        levels = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
        aidx = [min(range(8), key=lambda j: abs(levels[j] - v)) for v in a]
    abits = sum(i << (3 * n) for n, i in enumerate(aidx))
    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in px]
    hi, lo = px[lum.index(max(lum))], px[lum.index(min(lum))]
    def c565(p):
        return (p[0] * 31 // 255) << 11 | (p[1] * 63 // 255) << 5 | (p[2] * 31 // 255)
    def back(c):
        return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
    c0, c1 = c565(hi), c565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        cidx = [0] * 16
    else:
        e0, e1 = back(c0), back(c1)
        cs = [e0, e1, tuple((2 * e0[i] + e1[i]) // 3 for i in range(3)), tuple((e0[i] + 2 * e1[i]) // 3 for i in range(3))]
        cidx = [min(range(4), key=lambda j: sum((cs[j][i] - p[i]) ** 2 for i in range(3))) for p in px]
    cbits = sum(i << (2 * n) for n, i in enumerate(cidx))
    return bytes([a0, a1]) + abits.to_bytes(6, 'little') + struct.pack('<HHI', c0, c1, cbits)


def encode(img, header, payload_size):
    """RGBA -> .xds (header kept: same format and size), tiled as xds.decode untiles it."""
    w, h = img.size
    bw, bh = (w + 3) // 4, (h + 3) // 4
    padded = Image.new('RGBA', (bw * 4, bh * 4))
    padded.paste(img, (0, 0))
    px = padded.load()
    pitch, l = (bw + 31) & ~31, 4
    p = bytearray(payload_size)
    for by in range(bh):
        row = xds.outer(by, pitch, l)
        for bx in range(bw):
            off = xds.inner(bx, by, row, l)
            block = dxt5_block([px[bx * 4 + i % 4, by * 4 + i // 4] for i in range(16)])
            if off + 16 <= len(p):
                p[off:off + 16] = block
    p[0::2], p[1::2] = p[1::2], p[0::2]  # 8-in-16 endian
    return header + bytes(p)


def main():
    z = zipfile.ZipFile(sys.argv[1])
    os.makedirs(OUT, exist_ok=True)
    for name, source in (('C_TYP_Icon_RandomEvents', 'C_Map/C_TYP_Map/C_TYP_Icon_RaceType1.xds'),
                         ('C_TYP_Badge_RandomEvents', 'C_Map/C_TYP_Map/C_TYP_Badge_RaceType1.xds')):
        data = z.read(source)
        _, (fmt, w, h) = xds.decode(data)
        img = art(w, h)
        out = encode(img, data[:52], len(data) - 52)
        open(os.path.join(OUT, name + '.xds'), 'wb').write(out)
        print(name, w, h)
    # Roguelike Career's strikes, in place of the car level icons 0 and 1 (random_events.py)
    data = z.read('L_Map/Car_Level_Icons/CarLevelIcon0.xds')
    _, (fmt, w, h) = xds.decode(data)
    for n in (0, 1):
        img = strikes_art(w, h, n == 1)
        open(os.path.join(OUT, 'CarLevelIcon%d.xds' % n), 'wb').write(encode(img, data[:52], len(data) - 52))
        print('CarLevelIcon%d' % n, w, h)


if __name__ == '__main__':
    main()
