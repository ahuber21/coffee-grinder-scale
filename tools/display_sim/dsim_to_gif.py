#!/usr/bin/env python3
"""Converts a .dsim frame dump (see DisplaySimMain.h) into an animated GIF.

Usage: dsim_to_gif.py input.dsim output.gif [--scale N]
"""
import argparse
import struct

from PIL import Image


def rgb565_to_rgb888(pixel: int) -> tuple[int, int, int]:
    r5 = (pixel >> 11) & 0x1F
    g6 = (pixel >> 5) & 0x3F
    b5 = pixel & 0x1F
    r = (r5 * 255 + 15) // 31
    g = (g6 * 255 + 31) // 63
    b = (b5 * 255 + 15) // 31
    return r, g, b


def load_dsim(path: str):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"DSIM":
        raise ValueError(f"{path}: not a .dsim file (bad magic)")
    width, height, frame_count, fps = struct.unpack_from("<4I", data, 4)
    offset = 20
    pixels_per_frame = width * height
    frames = []
    for i in range(frame_count):
        start = offset + i * pixels_per_frame * 2
        raw = struct.unpack_from(f"<{pixels_per_frame}H", data, start)
        frames.append(raw)
    return width, height, fps, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--scale", type=int, default=4, help="integer upscale factor (default 4)")
    args = ap.parse_args()

    width, height, fps, frames = load_dsim(args.input)
    print(f"{args.input}: {len(frames)} frames, {width}x{height}, {fps}fps")

    palette_cache = {}
    images = []
    for raw in frames:
        img = Image.new("RGB", (width, height))
        px = img.load()
        for y in range(height):
            row_base = y * width
            for x in range(width):
                v = raw[row_base + x]
                rgb = palette_cache.get(v)
                if rgb is None:
                    rgb = rgb565_to_rgb888(v)
                    palette_cache[v] = rgb
                px[x, y] = rgb
        if args.scale > 1:
            img = img.resize((width * args.scale, height * args.scale), Image.NEAREST)
        images.append(img)

    duration_ms = round(1000 / fps)
    images[0].save(
        args.output,
        save_all=True,
        append_images=images[1:],
        duration=duration_ms,
        loop=0,
        optimize=False,
    )
    print(f"wrote {args.output}: {len(images)} frames @ {duration_ms}ms/frame")


if __name__ == "__main__":
    main()
