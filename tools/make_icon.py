#!/usr/bin/env python3
"""make_icon.py [--drawer] <logo.png> <out.info> [tooltype ...]

Writes an AmigaOS tool icon (.info) from an image:
  - the classic DiskObject with a 4-colour planar image (Workbench 1.x-3.1
    palette: grey, black, white, blue), dithered, as the fallback image
  - an OS 3.5+ colour icon ("GlowIcon": IFF FORM ICON with FACE and IMAG
    chunks, up to 256 colours, transparent colour 0), which the OS 3.2
    Workbench shows
  - the tool types given on the command line
All values big-endian, as icon.library reads them.
"""
import struct
import sys

from PIL import Image

ICON_W, ICON_H = 96, 48          # (largest size; the image keeps its aspect)
WB_PALETTE = [(170, 170, 170), (0, 0, 0), (255, 255, 255), (102, 136, 187)]


def fit(img):
    img = img.convert("RGBA")
    # crop to the clearly visible pixels (the soft glow around a logo turns
    # transparent in an icon and would only leave empty space)
    box = img.split()[3].point(lambda a: 255 if a >= 128 else 0).getbbox()
    if box:
        img = img.crop(box)
    img.thumbnail((ICON_W, ICON_H), Image.LANCZOS)
    return img


def classic_planes(rgba):
    """2 bitplanes, each row padded to 16 bits; transparent -> colour 0 (the background)"""
    w, h = rgba.size
    px = rgba.load()
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    idx = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                continue
            t = (bayer[y & 3][x & 3] - 7.5) * 6
            best, bd = 1, 1e18
            for i, (pr, pg, pb) in enumerate(WB_PALETTE):
                d = (r + t - pr) ** 2 + (g + t - pg) ** 2 + (b + t - pb) ** 2
                if d < bd:
                    best, bd = i, d
            idx[y][x] = best
    bpr = ((w + 15) // 16) * 2
    data = bytearray()
    for plane in range(2):
        for y in range(h):
            row = bytearray(bpr)
            for x in range(w):
                if (idx[y][x] >> plane) & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            data += row
    return data


def chunk(cid, body):
    out = cid + struct.pack(">I", len(body)) + body
    if len(body) & 1:
        out += b"\0"
    return out


def glow_icon(rgba):
    """FORM ICON: FACE + IMAG (uncompressed image and palette, colour 0 transparent)"""
    w, h = rgba.size
    alpha = rgba.split()[3]
    rgb = rgba.convert("RGB").quantize(colors=255, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
    pal = rgb.getpalette()[:255 * 3]
    src = rgb.load()
    ap = alpha.load()
    pixels = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            pixels[y * w + x] = 0 if ap[x, y] < 128 else src[x, y] + 1   # (0: transparent)
    palette = bytes([0, 0, 0]) + bytes(pal)
    ncol = len(palette) // 3
    face = struct.pack(">BBBBH", w - 1, h - 1, 0, 0, ncol * 3 - 1)
    imag = struct.pack(">BBBBBBHH", 0, ncol - 1, 0x03, 0, 0, 8, len(pixels) - 1, len(palette) - 1)
    imag += bytes(pixels) + palette
    body = b"ICON" + chunk(b"FACE", face) + chunk(b"IMAG", imag)
    return b"FORM" + struct.pack(">I", len(body)) + body


def disk_object(rgba, tooltypes, drawer=False):
    w, h = rgba.size
    planes = classic_planes(rgba)
    out = bytearray()
    # struct DiskObject (78 bytes) with its struct Gadget
    out += struct.pack(">HH", 0xE310, 1)                          # magic, version
    out += struct.pack(">IhhhhHHHIIIiIHI",
                       0,                 # NextGadget
                       0, 0, w, h,        # Left, Top, Width, Height
                       0x0005,            # Flags: GADGIMAGE | GADGHBOX
                       0x0003,            # Activation: RELVERIFY | GADGIMMEDIATE
                       0x0001,            # GadgetType: BOOLGADGET
                       1,                 # GadgetRender (non-zero: an image follows)
                       0,                 # SelectRender
                       0,                 # GadgetText
                       0,                 # MutualExclude
                       0,                 # SpecialInfo
                       0,                 # GadgetID
                       1)                 # UserData: OS 2.x icon revision
    out += struct.pack(">BB", 2 if drawer else 3, 0)              # do_Type WBDRAWER / WBTOOL, pad
    out += struct.pack(">II", 0, 1 if tooltypes else 0)           # DefaultTool, ToolTypes
    out += struct.pack(">ii", -0x80000000, -0x80000000)           # NO_ICON_POSITION
    out += struct.pack(">IIi", 1 if drawer else 0, 0, 0 if drawer else 262144)  # DrawerData, ToolWindow, Stack
    assert len(out) == 78
    if drawer:
        # struct DrawerData: struct NewWindow (48 bytes) + dd_CurrentX/Y
        out += struct.pack(">hhhhBBIIIIIIIhhhhH",
                           80, 60, 420, 180,      # window position and size
                           255, 255,              # pens
                           0, 0, 0, 0, 0, 0, 0,   # IDCMP, Flags, Gadget, CheckMark, Title, Screen, BitMap
                           90, 40, -1, -1,        # min / max size
                           1)                     # WBENCHSCREEN
        out += struct.pack(">ii", 0, 0)
    # struct Image (20 bytes) + planes
    out += struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 0x03, 0x00, 0)
    out += planes
    # tool types: (count + 1) * 4, then each string as length (with NUL) + bytes
    if tooltypes:
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            b = t.encode("latin-1") + b"\0"
            out += struct.pack(">I", len(b)) + b
    if drawer:
        out += struct.pack(">IH", 0, 0)   # DrawerData2 (OS 2.x icon): dd_Flags, dd_ViewModes = default
    return bytes(out)


def main():
    args = sys.argv[1:]
    drawer = bool(args) and args[0] == "--drawer"
    if drawer:
        args = args[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    rgba = fit(Image.open(args[0]))
    data = disk_object(rgba, args[2:], drawer) + glow_icon(rgba)
    with open(args[1], "wb") as f:
        f.write(data)
    print("%s: %d bytes, %dx%d" % (args[1], len(data), rgba.width, rgba.height))


if __name__ == "__main__":
    main()
