"""Generate all project-owned raster assets for Contact Dodger.

The generator uses only the Python standard library.  It deliberately avoids
system fonts and external media so the authored game is reproducible offline.
"""

from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def write_rgba_png(path: Path, width: int, height: int, pixels: bytearray) -> None:
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows.extend(pixels[y * stride : (y + 1) * stride])
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    encoded = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(encoded)


def set_pixel(pixels: bytearray, width: int, x: int, y: int, rgba: tuple[int, int, int, int]) -> None:
    offset = (y * width + x) * 4
    pixels[offset : offset + 4] = bytes(rgba)


def generate_sprites() -> None:
    width = height = 64
    pixels = bytearray(width * height * 4)

    # Tile 0: player arrow.
    for y in range(2, 14):
        for x in range(2, 14):
            if x >= abs(y - 8) + 3 or (x <= 6 and 5 <= y <= 10):
                set_pixel(pixels, width, x, y, (210, 245, 255, 255))

    # Tile 1: compact projectile with a hot core and two-pixel trail.
    for y in range(6, 11):
        for x in range(18, 30):
            edge = y in (6, 10) or x in (18, 29)
            set_pixel(pixels, width, x, y, (255, 220 if edge else 255, 80, 255))
    for x in range(16, 18):
        set_pixel(pixels, width, x, 8, (255, 120, 30, 210))

    # Tile 2: enemy diamond with a contrasting center.
    for y in range(1, 16):
        radius = 7 - abs(y - 8)
        for x in range(40 - radius, 41 + radius):
            color = (255, 80, 110, 255) if abs(x - 40) + abs(y - 8) > 3 else (90, 15, 45, 255)
            set_pixel(pixels, width, x, y, color)

    # Tile 3: eight-ray contact spark.
    for y in range(1, 16):
        for x in range(49, 64):
            dx, dy = x - 56, y - 8
            if dx == 0 or dy == 0 or abs(dx) == abs(dy) or dx * dx + dy * dy <= 9:
                set_pixel(pixels, width, x, y, (255, 250, 150, 255))

    # Tile 4: translucent scan gate.
    for y in range(18, 32):
        for x in range(3, 13):
            if x in (3, 12) or (x + y) % 4 == 0:
                set_pixel(pixels, width, x, y, (90, 245, 220, 150))

    write_rgba_png(ASSETS / "sprites.png", width, height, pixels)


GLYPHS = {
    " ": ["00000"] * 7,
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    ":": ["00000", "00100", "00100", "00000", "00100", "00100", "00000"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "10000", "11110", "00001", "00001", "11110"],
    "6": ["01110", "10000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00001", "01110"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01111", "10000", "10000", "10111", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "J": ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
}


def generate_font() -> None:
    columns, cell = 16, 8
    codepoints = list(range(32, 127))
    rows = (len(codepoints) + columns - 1) // columns
    width, height = columns * cell, rows * cell
    pixels = bytearray(width * height * 4)
    glyph_metadata = []

    for index, codepoint in enumerate(codepoints):
        character = chr(codepoint)
        pattern = GLYPHS.get(character.upper(), GLYPHS[" "])
        cell_x = (index % columns) * cell
        cell_y = (index // columns) * cell
        for y, row in enumerate(pattern):
            for x, bit in enumerate(row):
                if bit == "1":
                    set_pixel(pixels, width, cell_x + x + 1, cell_y + y, (235, 250, 255, 255))
        glyph_metadata.append(
            {
                "codepoint": codepoint,
                "uv": [cell_x / width, cell_y / height, cell / width, cell / height],
                "size": [6.0, 8.0],
                "bearing": [0.0, 8.0],
                "advance": 6.0,
            }
        )

    write_rgba_png(ASSETS / "font.png", width, height, pixels)
    (ASSETS / "font.json").write_text(
        json.dumps({"schema_version": "1", "line_height": 8.0, "glyphs": glyph_metadata}, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    generate_sprites()
    generate_font()
    print("generated assets/sprites.png assets/font.png assets/font.json")


if __name__ == "__main__":
    main()
