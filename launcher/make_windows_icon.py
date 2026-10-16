#!/usr/bin/env python3
import struct
import sys
from pathlib import Path


def decode_tga(data: bytes):
    if len(data) < 18:
        raise ValueError("TGA header is too short")
    ident = data[0]
    cmap = data[1]
    kind = data[2]
    width = data[12] | (data[13] << 8)
    height = data[14] | (data[15] << 8)
    bits = data[16]
    descriptor = data[17]
    if cmap != 0 or kind not in (2, 10) or bits not in (24, 32) or not width or not height:
        raise ValueError("unsupported TGA format")

    step = bits // 8
    at = 18 + ident
    count = width * height
    pixels = []

    def pixel(pos):
        b, g, r = data[pos:pos + 3]
        a = data[pos + 3] if step == 4 else 255
        return (r, g, b, a)

    while len(pixels) < count:
        if kind == 10:
            if at >= len(data):
                raise ValueError("truncated TGA")
            header = data[at]
            at += 1
            run = (header & 0x7F) + 1
            repeat = bool(header & 0x80)
        else:
            run = 1
            repeat = False

        if len(pixels) + run > count:
            raise ValueError("invalid TGA run")

        if repeat:
            if at + step > len(data):
                raise ValueError("truncated TGA")
            value = pixel(at)
            at += step
            pixels.extend([value] * run)
        else:
            for _ in range(run):
                if at + step > len(data):
                    raise ValueError("truncated TGA")
                pixels.append(pixel(at))
                at += step

    out = [None] * count
    for y in range(height):
        row = y if descriptor & 0x20 else height - 1 - y
        for x in range(width):
            col = width - 1 - x if descriptor & 0x10 else x
            out[y * width + x] = pixels[row * width + col]
    return width, height, out


def resize_nearest(width, height, pixels, limit=256):
    if width <= limit and height <= limit:
        return width, height, pixels
    scale = min(limit / width, limit / height)
    nw = max(1, int(width * scale))
    nh = max(1, int(height * scale))
    out = []
    for y in range(nh):
        sy = min(height - 1, int(y * height / nh))
        for x in range(nw):
            sx = min(width - 1, int(x * width / nw))
            out.append(pixels[sy * width + sx])
    return nw, nh, out


def make_ico(width, height, pixels):
    # 32-bit DIB inside an ICO. Height in BITMAPINFOHEADER includes XOR + AND.
    xor = bytearray()
    for y in range(height - 1, -1, -1):
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            xor.extend((b, g, r, a))

    mask_stride = ((width + 31) // 32) * 4
    and_mask = bytearray(mask_stride * height)
    for out_row, y in enumerate(range(height - 1, -1, -1)):
        for x in range(width):
            if pixels[y * width + x][3] < 128:
                byte = out_row * mask_stride + x // 8
                and_mask[byte] |= 0x80 >> (x % 8)

    dib = struct.pack(
        "<IIIHHIIIIII",
        40, width, height * 2, 1, 32, 0,
        len(xor), 0, 0, 0, 0
    ) + xor + and_mask

    header = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack(
        "<BBBBHHII",
        0 if width >= 256 else width,
        0 if height >= 256 else height,
        0, 0, 1, 32, len(dib), 6 + 16
    )
    return header + entry + dib


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_windows_icon.py INPUT.tga OUTPUT.ico")
    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    width, height, pixels = decode_tga(source.read_bytes())
    width, height, pixels = resize_nearest(width, height, pixels)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(make_ico(width, height, pixels))
    print(f"wrote {target} ({width}x{height})")


if __name__ == "__main__":
    main()
