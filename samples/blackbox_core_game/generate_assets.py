"""Generate all Pulse Salvage bitmap assets using only the Python standard library."""

from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def write_png(path: Path, width: int, height: int, rgba: bytearray) -> None:
    rows = []
    stride = width * 4
    for y in range(height):
        rows.append(b"\x00" + bytes(rgba[y * stride : (y + 1) * stride]))
    encoded = b"\x89PNG\r\n\x1a\n"
    encoded += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    encoded += png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    encoded += png_chunk(b"IEND", b"")
    path.write_bytes(encoded)


def pixel(image: bytearray, width: int, height: int, x: int, y: int, color: tuple[int, int, int, int]) -> None:
    if 0 <= x < width and 0 <= y < height:
        offset = (y * width + x) * 4
        image[offset : offset + 4] = bytes(color)


def rect(
    image: bytearray,
    width: int,
    height: int,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    color: tuple[int, int, int, int],
) -> None:
    for y in range(y0, y1):
        for x in range(x0, x1):
            pixel(image, width, height, x, y, color)


def generate_sprites() -> None:
    width, height = 128, 16
    image = bytearray(width * height * 4)

    tile_colors = [
        (8, 13, 34, 255),
        (35, 210, 235, 255),
        (245, 65, 92, 255),
        (112, 74, 180, 255),
        (255, 205, 62, 255),
        (240, 250, 255, 220),
        (12, 74, 56, 255),
        (72, 10, 28, 255),
    ]
    for tile, color in enumerate(tile_colors):
        x0 = tile * 16
        rect(image, width, height, x0, 0, x0 + 16, 16, color)

    # Deep-space background with deterministic stars.
    for x, y in [(2, 3), (7, 12), (12, 5), (4, 8), (14, 14)]:
        pixel(image, width, height, x, y, (100, 155, 230, 255))

    # Cyan probe: diamond hull, dark cockpit, and two engine pixels.
    x0 = 16
    rect(image, width, height, x0, 0, x0 + 16, 16, (0, 0, 0, 0))
    for y in range(2, 14):
        half = min(y - 1, 14 - y, 6)
        rect(image, width, height, x0 + 8 - half, y, x0 + 9 + half, y + 1, (36, 220, 238, 255))
    rect(image, width, height, x0 + 6, 5, x0 + 11, 9, (10, 48, 88, 255))
    pixel(image, width, height, x0 + 4, 14, (255, 205, 60, 255))
    pixel(image, width, height, x0 + 12, 14, (255, 205, 60, 255))

    # Red hazard orb with a bright core.
    x0 = 32
    rect(image, width, height, x0, 0, x0 + 16, 16, (0, 0, 0, 0))
    for y in range(2, 14):
        half = 5 if 4 <= y <= 11 else 3
        rect(image, width, height, x0 + 8 - half, y, x0 + 9 + half, y + 1, (235, 45, 78, 255))
    rect(image, width, height, x0 + 6, 6, x0 + 10, 10, (255, 220, 110, 255))

    # Purple energy wall.
    x0 = 48
    for y in range(16):
        color = (146, 92, 218, 255) if y % 2 == 0 else (84, 54, 150, 255)
        rect(image, width, height, x0, y, x0 + 16, y + 1, color)

    # Yellow packet.
    x0 = 64
    rect(image, width, height, x0, 0, x0 + 16, 16, (0, 0, 0, 0))
    rect(image, width, height, x0 + 3, 3, x0 + 13, 13, (255, 210, 52, 255))
    rect(image, width, height, x0 + 6, 6, x0 + 10, 10, (255, 250, 185, 255))

    # White contact burst.
    x0 = 80
    rect(image, width, height, x0, 0, x0 + 16, 16, (0, 0, 0, 0))
    for d in range(2, 14):
        pixel(image, width, height, x0 + 8, d, (235, 250, 255, 230))
        pixel(image, width, height, x0 + d, 8, (235, 250, 255, 230))
    for d in range(4, 12):
        pixel(image, width, height, x0 + d, d, (125, 235, 255, 210))
        pixel(image, width, height, x0 + d, 15 - d, (125, 235, 255, 210))

    # Victory and defeat backdrops get simple scanlines and corner accents.
    for tile, accent in [(6, (70, 220, 150, 255)), (7, (240, 72, 88, 255))]:
        x0 = tile * 16
        for y in range(0, 16, 2):
            rect(image, width, height, x0, y, x0 + 16, y + 1, accent)
        for p in range(5):
            pixel(image, width, height, x0 + 2 + p, 2, (255, 255, 255, 180))
            pixel(image, width, height, x0 + 13 - p, 13, (255, 255, 255, 180))

    write_png(ASSETS / "sprites.png", width, height, image)


FONT_5X7 = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "10010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    "{": ("00110", "00100", "00100", "11000", "00100", "00100", "00110"),
    "}": ("01100", "00100", "00100", "00011", "00100", "00100", "01100"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "_": ("00000", "00000", "00000", "00000", "00000", "00000", "11111"),
}


def fallback_pattern(codepoint: int) -> tuple[str, ...]:
    rows = []
    for y in range(7):
        row = ""
        for x in range(5):
            border = x in (0, 4) or y in (0, 6)
            bit = (codepoint >> ((x + y * 2) % 7)) & 1
            row += "1" if border and bit else "0"
        rows.append(row)
    return tuple(rows)


def generate_font() -> None:
    cell_width, cell_height = 8, 10
    columns = 16
    codepoints = list(range(32, 127))
    rows = (len(codepoints) + columns - 1) // columns
    width, height = columns * cell_width, rows * cell_height
    image = bytearray(width * height * 4)
    glyphs = []

    for index, codepoint in enumerate(codepoints):
        character = chr(codepoint)
        column = index % columns
        row = index // columns
        x0, y0 = column * cell_width + 1, row * cell_height + 1
        lookup = character.upper() if character.isalpha() else character
        pattern = FONT_5X7.get(lookup, fallback_pattern(codepoint))
        if character != " ":
            for y, bits in enumerate(pattern):
                for x, bit in enumerate(bits):
                    if bit == "1":
                        pixel(image, width, height, x0 + x, y0 + y, (238, 247, 255, 255))
        glyphs.append(
            {
                "codepoint": codepoint,
                "uv": [
                    (column * cell_width) / width,
                    (row * cell_height) / height,
                    cell_width / width,
                    cell_height / height,
                ],
                "size": [cell_width, cell_height],
                "bearing": [0, 0],
                "advance": 6,
            }
        )

    write_png(ASSETS / "font.png", width, height, image)
    metadata = {"schema_version": "1", "line_height": 10, "glyphs": glyphs}
    (ASSETS / "font.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    generate_sprites()
    generate_font()
    print("generated assets/sprites.png, assets/font.png, and assets/font.json")


if __name__ == "__main__":
    main()
