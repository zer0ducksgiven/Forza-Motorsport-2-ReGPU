"""Builds src/gpu/ps_glyphs.h: PlayStation button prompts for FM2's Xbox ones (shown while a
PlayStation pad is in use; see src/gpu/renderer.cpp).

  ps_glyphs.py <game folder> <kenney_input-prompts zip> <DualSense picture png> <Xbox Series picture png> [out.h]

The controller pictures (the options screen's controller layout) are "Solid Color 4k.png" from
"PS5 Button Icons and Controls" and "Xbox Series Button Icons and Controls" by Zacksly
(https://zacksly.itch.io, CC BY 3.0), resized.

Each Xbox prompt texture in Media/UI/Textures.zip is identified by its size, format and an FNV-1a
hash of its data (as it sits in guest memory; nothing of the game's art is stored). Its replacement
is Kenney's white PlayStation icon (Input Prompts 1.5, CC0: kenney.nl/assets/input-prompts),
fitted to where the Xbox icon sits in that texture, with a gloss and a drop shadow to match FM2's.
"""
import io
import os
import struct
import sys
import zipfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xds import decode  # noqa: E402

# FM2 texture -> Kenney icons (side by side when several), from PlayStation Series/Double.
MAP = {
    "P_Map/P_OPT/P_Button_A.xds": ["playstation_button_cross"],
    "P_Map/P_OPT/P_Button_B.xds": ["playstation_button_circle"],
    "P_Map/P_OPT/P_Button_X.xds": ["playstation_button_square"],
    "P_Map/P_OPT/P_Button_Y.xds": ["playstation_button_triangle"],
    "I_Map/I_CAM/Xbutton.xds": ["playstation_button_square"],
    "I_Map/I_CAM/Ybutton.xds": ["playstation_button_triangle"],
    "X_Map/X_AUC/X_AUC_LargeAButton.xds": ["playstation_button_cross"],
    "X_Map/X_AUC/X_AUC_StartButtonA.xds": ["playstation_button_cross"],
    # (The file names are swapped: _LeftShoulder_n shows RB.)
    "I_Map/I_REP_Telemetry/Butn_LeftShoulder_n.xds": ["playstation_trigger_r1"],
    "I_Map/I_REP_Telemetry/Butn_RightShoulder_n.xds": ["playstation_trigger_l1"],
    "P_Map/P_OPT/P_Button_Triggers.xds": ["playstation_trigger_l2", "playstation_trigger_r2"],
    "P_Map/P_OPT/P_Button_Thumb.xds": ["playstation_button_l3"],
    "I_Map/I_CAM/Start.xds": ["playstation5_button_options_alternative"],
    # Photo mode's controller pictures: the stick (and L1 / R1) they point at.
    "I_Map/I_CAM/DPad.xds": ["playstation_dpad"],
    "I_Map/I_CAM/LeftThumb.xds": ["playstation_stick_l"],
    "I_Map/I_CAM/RightThumb.xds": ["playstation_stick_r"],
    "I_Map/I_CAM/LeftThumb_ShoulderButtons.xds": ["playstation_stick_l", "playstation_trigger_l1", "playstation_trigger_r1"],
    "I_Map/I_CAM/RightThumb_ShoulderButtons.xds": ["playstation_stick_r", "playstation_trigger_l1", "playstation_trigger_r1"],
    # The home badge in the corner of the career screens, and its counterpart.
    "C_Map/C_MYC_Map/C_MYC_MessCtr_RB.xds": ["playstation_trigger_r1"],
    "C_Map/C_MYC_Map/C_MYC_MessCtr_LB.xds": ["playstation_trigger_l1"],
}
# The options screen's controller-layout pictures: the whole DualSense, fitted to the 360's outline.
DIAGRAMS = ["P_Map/P_OPT/P_360Controller.xds", "P_Map/P_OPT/P_360Controller2.xds",
            "P_Map/P_OPT/P_OPT_ICON_controller.xds", "P_Map/P_OPT/P_OPT_ICON_controllerLayout.xds"]
# The same prompts for a modern Xbox pad (One / Series): Zacksly's "Buttons Outline/Black" icons,
# filled (see zacksly_icon), PlayStation name -> Zacksly file name.
SERIES = {
    "playstation_button_cross": "A", "playstation_button_circle": "B",
    "playstation_button_square": "X", "playstation_button_triangle": "Y",
    "playstation_trigger_l1": "Left Bumper", "playstation_trigger_r1": "Right Bumper",
    "playstation_trigger_l2": "Left Trigger", "playstation_trigger_r2": "Right Trigger",
    "playstation_button_l3": "Left Stick Click", "playstation5_button_options_alternative": "Menu",
    "playstation_dpad": "D-Pad", "playstation_stick_l": "Left Stick", "playstation_stick_r": "Right Stick",
}
# Fills for the Series icons: the face buttons in the pad's colours, the rest light grey.
SERIES_FILL = {"A": (110, 190, 60), "B": (230, 60, 50), "X": (40, 130, 230), "Y": (250, 200, 30)}
# Textures with more than the button (an envelope, a house): only the button's part, this box (as
# fractions of the texture), is replaced; the rest is the game's own, decoded at run time.
OVERLAY = {
    "C_Map/C_MYC_Map/C_MYC_LB_Msg_icon.xds": (["playstation_trigger_l1"], (0.0, 0.0, 0.56, 1.0)),
    "C_Map/C_MYC_Map/C_MYC_Home_Icon.xds": (["playstation_trigger_l1"], (0.0, 0.38, 0.52, 0.78)),
}


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def gloss(icon):
    """FM2's prompts are shiny: a light top, a darker bottom, a soft shadow below."""
    w, h = icon.size
    r, g, b, a = icon.split()
    shade = Image.new("L", (w, h))
    shade.putdata([int(255 - 45 * (y / max(h - 1, 1)) ** 1.5) for y in range(h) for _ in range(w)])
    rgb = Image.merge("RGB", (r, g, b))
    rgb = ImageChops.multiply(rgb, Image.merge("RGB", (shade, shade, shade)))
    # A highlight band across the top third.
    hl = Image.new("L", (w, h), 0)
    ImageDraw.Draw(hl).ellipse((-w * 0.1, -h * 0.55, w * 1.1, h * 0.42), fill=70)
    hl = ImageChops.multiply(hl.filter(ImageFilter.GaussianBlur(max(1, w // 16))), a)
    rgb = ImageChops.add(rgb, Image.merge("RGB", (hl, hl, hl)))
    return Image.merge("RGBA", (*rgb.split(), a))


def outlined(icon):
    """At final size: a thin mid-grey edge and a soft shadow below, so white icons read on the
    career mode's white backgrounds without looking heavy."""
    w, h = icon.size
    unit = max(1, round(min(w, h) / 32))
    pad = unit * 4
    a = Image.new("L", (w + pad * 2, h + pad * 2), 0)
    a.paste(icon.split()[3], (pad, pad))
    out = Image.new("RGBA", a.size, (0, 0, 0, 0))
    shadow = Image.new("RGBA", a.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 120), (unit, unit * 2), a)
    out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(1.6 * unit)))
    edge = Image.new("RGBA", a.size, (0, 0, 0, 0))
    edge.paste((90, 90, 90, 200), (0, 0), a.filter(ImageFilter.MaxFilter(unit * 2 + 1)))
    out.alpha_composite(edge)
    out.alpha_composite(icon, (pad, pad))
    return out, pad


def build(original, icons, region=None, shine=True):
    """The replacement for one texture: its size, the icons fitted to the Xbox icon's box (within
    region, fractions of the texture, when given)."""
    w, h = original.size
    alpha = original.split()[3].point(lambda v: 255 if v > 40 else 0)
    if region:
        r = (int(region[0] * w), int(region[1] * h), int(region[2] * w), int(region[3] * h))
        sub = alpha.crop(r).getbbox()
        box = (r[0] + sub[0], r[1] + sub[1], r[0] + sub[2], r[1] + sub[3]) if sub else r
    else:
        box = alpha.getbbox() or (0, 0, w, h)
    # The outline and shadow need a margin: the box is kept that far inside the texture.
    m = max(2, round(min(box[2] - box[0], box[3] - box[1]) / 8))
    box = (max(box[0], m), max(box[1], m), min(box[2], w - m), min(box[3], h - m))
    bw, bh = box[2] - box[0], box[3] - box[1]
    glossy = [gloss(i) if shine else i for i in icons]
    gap = max(1, bh // 8)
    total = sum(i.width * bh / i.height for i in glossy) + gap * (len(glossy) - 1)
    scale = min(1.0, bw / total) if total else 1.0
    th = max(1, int(bh * scale))
    x = box[0] + (bw - int(total * scale)) // 2
    y = box[1] + (bh - th) // 2
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for i in glossy:
        tw = max(1, int(i.width * th / i.height))
        icon, pad = outlined(i.resize((tw, th), Image.LANCZOS))
        out.alpha_composite(icon, (x - pad, y - pad)) if x >= pad and y >= pad else out.alpha_composite(icon.crop((max(0, pad - x), max(0, pad - y), icon.width, icon.height)), (max(0, x - pad), max(0, y - pad)))
        x += tw + int(gap * scale)
    return out

def zacksly_icon(path, fill):
    """A Zacksly outline icon filled: the shape in fill, its letters and rim dark."""
    im = Image.open(path).convert("RGBA")
    a = im.split()[3]
    w, h = im.size
    f = Image.new("L", (w + 4, h + 4), 0)
    f.paste(a.point(lambda v: 255 if v > 100 else 0), (2, 2))
    ImageDraw.floodfill(f, (0, 0), 128)
    shape = f.point(lambda v: 0 if v == 128 else 255).crop((2, 2, w + 2, h + 2))
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    out.paste((24, 24, 28, 255), (0, 0), shape)
    out.paste(fill + (255,), (0, 0), ImageChops.subtract(shape, a))
    return out


def build_set(tex, kz, folder, rename, art):
    """One set's entries: (name, w, h, fmt, bytes, hash, image, clear) per Xbox texture."""
    entries = []
    jobs = [(n, i, None) for n, i in MAP.items()] + [(n, i, r) for n, (i, r) in OVERLAY.items()]
    if art is not None:
        jobs += [(n, None, None) for n in DIAGRAMS]
    for name, icons, region in jobs:
        data = tex.read(name)
        hdr = struct.unpack(">13I", data[:52])
        fmt, size = hdr[8] & 0x3F, hdr[9]
        w, h = (size & 0x1FFF) + 1, ((size >> 13) & 0x1FFF) + 1
        original, _ = decode(data)
        if icons is None:  # a whole controller picture
            box = original.split()[3].point(lambda v: 255 if v > 8 else 0).getbbox()
            bw, bh = box[2] - box[0], box[3] - box[1]
            sc = min(bw / art.width, bh / art.height)
            fw, fh = int(art.width * sc), int(art.height * sc)
            img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            img.alpha_composite(art.resize((fw, fh), Image.LANCZOS), (box[0] + (bw - fw) // 2, box[1] + (bh - fh) // 2))
            entries.append((name, w, h, fmt, len(data) - 52, fnv1a(data[52:]), img, (0, 0, 0, 0)))
            continue
        if os.path.isdir(folder):  # Zacksly's icons
            images = [zacksly_icon(os.path.join(folder, "%s.png" % rename[i]), SERIES_FILL.get(rename[i], (235, 235, 235))) for i in icons]
        else:
            images = [Image.open(io.BytesIO(kz.read("%s/Double/%s.png" % (folder, i)))).convert("RGBA") for i in icons]
        images = [i.crop(i.split()[3].getbbox()) for i in images]  # the tiles have a margin
        img = build(original, images, region, not os.path.isdir(folder))
        clear = (0, 0, 0, 0)
        if region:  # the pill's box: cleared (with a margin) before the icon is drawn over it
            r = (int(region[0] * w), int(region[1] * h), int(region[2] * w), int(region[3] * h))
            sub = original.split()[3].point(lambda v: 255 if v > 40 else 0).crop(r).getbbox() or (0, 0, r[2] - r[0], r[3] - r[1])
            clear = (max(0, r[0] + sub[0] - 3), max(0, r[1] + sub[1] - 3), min(w, r[0] + sub[2] + 3), min(h, r[1] + sub[3] + 3))
        entries.append((name, w, h, fmt, len(data) - 52, fnv1a(data[52:]), img, clear))
    return entries


def emit(lines, prefix, table, entries):
    for n, (name, w, h, fmt, nbytes, hsh, img, clear) in enumerate(entries):
        raw = img.tobytes()
        lines.append("// %s" % name)
        lines.append("inline const uint8_t %s%d[] = {" % (prefix, n))
        for i in range(0, len(raw), 32):
            lines.append("    " + ", ".join(str(b) for b in raw[i:i + 32]) + ",")
        lines.append("};")
    lines.append("inline const PsGlyph %s[] = {" % table)
    for n, (name, w, h, fmt, nbytes, hsh, img, clear) in enumerate(entries):
        lines.append("    { %d, %d, 0x%X, %d, 0x%016Xull, %s%d, { %d, %d, %d, %d } }, // %s" % (w, h, fmt, nbytes, hsh, prefix, n, *clear, name.split("/")[-1]))
    lines.append("};")


def main(argv):
    game, kenney, diagram, series_diagram = argv[1], argv[2], argv[3], argv[4]
    out_path = argv[5] if len(argv) > 5 else os.path.join(os.path.dirname(__file__), "..", "src", "gpu", "ps_glyphs.h")
    tex = zipfile.ZipFile(os.path.join(game, "Media", "UI", "Textures.zip"))
    kz = zipfile.ZipFile(kenney)
    art = Image.open(diagram).convert("RGBA")
    art = art.crop(art.split()[3].getbbox())
    entries = build_set(tex, kz, "PlayStation Series", {}, art)
    series_art = Image.open(series_diagram).convert("RGBA")
    series_art = series_art.crop(series_art.split()[3].getbbox())
    zk = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(series_diagram))), "Buttons Outline", "Black", "256w")
    series = build_set(tex, kz, zk, SERIES, series_art)
    lines = ["// Generated by tools/ps_glyphs.py: PlayStation and Xbox Series button prompts for FM2's Xbox 360 ones.",
             "// Icons: Kenney Input Prompts 1.5 (CC0, kenney.nl/assets/input-prompts), glossed (PlayStation);",
             "// Zacksly's Xbox Series ones (CC BY 3.0), filled.",
             "// Controller picture: \"PS5 Button Icons and Controls\" and \"Xbox Series Button Icons and Controls\"",
             "// by Zacksly (zacksly.itch.io, CC BY 3.0), resized.",
             "#pragma once", "#include <cstdint>", "",
             "struct PsGlyph", "{",
             "    uint32_t width, height, format, bytes; // the Xbox texture: size, Xenos format, data bytes",
             "    uint64_t hash;                         // FNV-1a of those bytes",
             "    const uint8_t* rgba;                   // the replacement, width x height RGBA8",
             "    uint32_t clear[4];                     // overlay: the game's texture with this box cleared, rgba drawn over it",
             "                                           // (all zero: rgba replaces it)", "};", ""]
    emit(lines, "kPsGlyph", "kPsGlyphs", entries)
    emit(lines, "kSeriesGlyph", "kSeriesGlyphs", series)
    open(out_path, "w", newline="\n").write("\n".join(lines) + "\n")
    for tag, ents in (("ps", entries), ("series", series)):
        sheet = Image.new("RGBA", (sum(e[6].width + 8 for e in ents) * 2, 290), (200, 200, 200, 255))
        x = 4
        for e in ents:
            orig, _ = decode(tex.read(e[0]))
            sheet.alpha_composite(orig, (x, 4))
            if any(e[7]):
                orig = orig.copy()
                ImageDraw.Draw(orig).rectangle((e[7][0], e[7][1], e[7][2] - 1, e[7][3] - 1), fill=(0, 0, 0, 0))
                orig.alpha_composite(e[6])
                sheet.alpha_composite(orig, (x, 72 + 70))
            else:
                sheet.alpha_composite(e[6], (x, 72))
            x += e[6].width + 8
        sheet.save(os.path.splitext(out_path)[0] + "_preview_%s.png" % tag)
    print("%d + %d glyphs -> %s" % (len(entries), len(series), out_path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
