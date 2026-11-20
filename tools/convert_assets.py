#!/usr/bin/env python3
"""Converts the freegemas media for the Amiga port (runs on the host).

  PNG -> .gfx : "FGX2" (all big-endian)
                u16 w, u16 h, u8 kind (0 opaque, 1 hard mask, 2 soft alpha), u8 0,
                i16 bx0, by0, bx1, by1 (bounding box of the visible pixels), u16 0
                w*h RGB565 (4x4 ordered dither)
                kind 1: the 1-bit mask, (w+7)/8 bytes per row, MSB = left pixel
                kind 2: w*h alpha bytes
                kind 1/2: the row runs: u32 count, u32 rowStart[h+1],
                          u16 run[count] = kind << 14 | length (0 skip, 1 blend, 2 copy)
  OGG -> .snd : raw 8-bit signed mono PCM at 22050 Hz (decoded by ffmpeg)
  TTF         : copied unchanged

usage: convert_assets.py <upstream/media> <out/media>
"""
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

RATE = 22050

# 4x4 ordered dither: hides the RGB565 banding of the gradients
BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]


# Sprites that move over changing ground (gems on the board and in the menu,
# the hand cursor): a hard 1-bit mask (alpha >= 50 % visible) -- they are
# plain cookie-cut blits then, no blending. Their soft edge pixels are the dark
# outline, which works on every ground. Everything else keeps its alpha: it
# rests on a fixed ground and is blended once into the static layer.
HARD_MASK = {"gemBlue", "gemGreen", "gemOrange", "gemPurple", "gemRed", "gemWhite", "gemYellow",
             "handCursor"}


def row_runs(alpha, w, h):
    """per row: runs of (kind << 14 | length), kind 0 skip, 1 blend, 2 copy (as render.cpp)"""
    runs, starts = [], []
    for y in range(h):
        starts.append(len(runs))
        x = 0
        while x < w:
            v = alpha[y * w + x]
            k = 0 if v == 0 else (2 if v == 255 else 1)
            s = x
            while x < w and x - s < 16383:
                v = alpha[y * w + x]
                if (0 if v == 0 else (2 if v == 255 else 1)) != k:
                    break
                x += 1
            runs.append((k << 14) | (x - s))
    starts.append(len(runs))
    return runs, starts


def convert_png(src, dst):
    hard = os.path.splitext(os.path.basename(src))[0] in HARD_MASK
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    px = im.tobytes()
    rgb = bytearray(w * h * 2)
    alpha = bytearray(w * h)
    for i in range(w * h):
        r, g, b, a = px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3]
        if hard:
            a = 255 if a >= 128 else 0
        if a == 0:
            r = g = b = 0
        else:
            d = BAYER[((i // w) & 3) * 4 + ((i % w) & 3)]
            r = min(255, r + (d >> 1))
            g = min(255, g + (d >> 2))
            b = min(255, b + (d >> 1))
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb[2 * i] = v >> 8
        rgb[2 * i + 1] = v & 0xFF
        alpha[i] = a
    values = set(alpha)
    kind = 0 if values == {255} else (1 if values <= {0, 255} else 2)
    xs = [i % w for i in range(w * h) if alpha[i]]
    ys = [i // w for i in range(w * h) if alpha[i]]
    box = (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else (0, 0, 0, 0)
    with open(dst, "wb") as f:
        f.write(b"FGX2")
        f.write(struct.pack(">HHBBhhhhH", w, h, kind, 0, *box, 0))
        f.write(rgb)
        if kind == 1:
            bpr = (w + 7) // 8
            mask = bytearray(bpr * h)
            for i in range(w * h):
                if alpha[i]:
                    y, x = divmod(i, w)
                    mask[y * bpr + (x >> 3)] |= 0x80 >> (x & 7)
            f.write(mask)
        elif kind == 2:
            f.write(alpha)
        if kind:
            runs, starts = row_runs(alpha, w, h)
            f.write(struct.pack(">I", len(runs)))
            f.write(struct.pack(">%dI" % len(starts), *starts))
            f.write(struct.pack(">%dH" % len(runs), *runs))
    return w, h, ("opaque", "hard mask", "alpha")[kind]


def convert_ogg(src, dst):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", str(RATE),
                    "-af", "aresample=resampler=soxr", "-f", "s8", dst], check=True)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src_root, dst_root = sys.argv[1], sys.argv[2]
    for dirpath, _, files in os.walk(src_root):
        rel = os.path.relpath(dirpath, src_root)
        out_dir = os.path.normpath(os.path.join(dst_root, rel))
        os.makedirs(out_dir, exist_ok=True)
        for name in sorted(files):
            src = os.path.join(dirpath, name)
            base, ext = os.path.splitext(name)
            ext = ext.lower()
            if ext == ".png":
                dst = os.path.join(out_dir, base + ".gfx")
                w, h, k = convert_png(src, dst)
                kind = "%dx%d %s" % (w, h, k)
            elif ext == ".ogg":
                dst = os.path.join(out_dir, base + ".snd")
                convert_ogg(src, dst)
                kind = "%d samples" % os.path.getsize(dst)
            elif ext == ".ttf":
                dst = os.path.join(out_dir, name)
                shutil.copyfile(src, dst)
                kind = "font"
            else:
                continue
            print("  %-40s %s" % (os.path.relpath(dst, dst_root), kind))


if __name__ == "__main__":
    main()
