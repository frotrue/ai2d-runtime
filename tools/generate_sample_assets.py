#!/usr/bin/env python3
"""Generate project-owned procedural v0.3 sample sprites and a 5x7 ASCII font."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parents[1]
SAMPLES = ("snake", "grid_collector")


FONT: dict[str, tuple[str, ...]] = {
    " ": ("00000",) * 7,
    "!": ("00100", "00100", "00100", "00100", "00100", "00000", "00100"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
}


def rows(*values: str) -> tuple[str, ...]:
    return values


FONT.update({
    "0": rows("01110","10001","10011","10101","11001","10001","01110"),
    "1": rows("00100","01100","00100","00100","00100","00100","01110"),
    "2": rows("01110","10001","00001","00010","00100","01000","11111"),
    "3": rows("11110","00001","00001","01110","00001","00001","11110"),
    "4": rows("00010","00110","01010","10010","11111","00010","00010"),
    "5": rows("11111","10000","10000","11110","00001","00001","11110"),
    "6": rows("01110","10000","10000","11110","10001","10001","01110"),
    "7": rows("11111","00001","00010","00100","01000","01000","01000"),
    "8": rows("01110","10001","10001","01110","10001","10001","01110"),
    "9": rows("01110","10001","10001","01111","00001","00001","01110"),
    "A": rows("01110","10001","10001","11111","10001","10001","10001"),
    "B": rows("11110","10001","10001","11110","10001","10001","11110"),
    "C": rows("01111","10000","10000","10000","10000","10000","01111"),
    "D": rows("11110","10001","10001","10001","10001","10001","11110"),
    "E": rows("11111","10000","10000","11110","10000","10000","11111"),
    "F": rows("11111","10000","10000","11110","10000","10000","10000"),
    "G": rows("01111","10000","10000","10111","10001","10001","01111"),
    "H": rows("10001","10001","10001","11111","10001","10001","10001"),
    "I": rows("01110","00100","00100","00100","00100","00100","01110"),
    "J": rows("00001","00001","00001","00001","10001","10001","01110"),
    "K": rows("10001","10010","10100","11000","10100","10010","10001"),
    "L": rows("10000","10000","10000","10000","10000","10000","11111"),
    "M": rows("10001","11011","10101","10101","10001","10001","10001"),
    "N": rows("10001","11001","10101","10011","10001","10001","10001"),
    "O": rows("01110","10001","10001","10001","10001","10001","01110"),
    "P": rows("11110","10001","10001","11110","10000","10000","10000"),
    "Q": rows("01110","10001","10001","10001","10101","10010","01101"),
    "R": rows("11110","10001","10001","11110","10100","10010","10001"),
    "S": rows("01111","10000","10000","01110","00001","00001","11110"),
    "T": rows("11111","00100","00100","00100","00100","00100","00100"),
    "U": rows("10001","10001","10001","10001","10001","10001","01110"),
    "V": rows("10001","10001","10001","10001","10001","01010","00100"),
    "W": rows("10001","10001","10001","10101","10101","11011","10001"),
    "X": rows("10001","10001","01010","00100","01010","10001","10001"),
    "Y": rows("10001","10001","01010","00100","00100","00100","00100"),
    "Z": rows("11111","00001","00010","00100","01000","10000","11111"),
})


def png_chunk(name: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xFFFFFFFF)


def write_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    raw = b"".join(b"\x00" + pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


def set_pixel(pixels: bytearray, width: int, x: int, y: int, color: tuple[int, int, int, int]) -> None:
    offset = (y * width + x) * 4
    pixels[offset : offset + 4] = bytes(color)


def make_font(directory: Path) -> None:
    characters = " !-:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    cell_w, cell_h, scale, columns = 8, 10, 1, 16
    rows_count = (len(characters) + columns - 1) // columns
    width, height = columns * cell_w, rows_count * cell_h
    pixels = bytearray(width * height * 4)
    glyphs: list[dict[str, object]] = []
    for index, character in enumerate(characters):
        x0 = (index % columns) * cell_w + 1
        y0 = (index // columns) * cell_h + 1
        pattern = FONT[character]
        for y, row in enumerate(pattern):
            for x, value in enumerate(row):
                if value == "1":
                    set_pixel(pixels, width, x0 + x * scale, y0 + y * scale, (255, 255, 255, 255))
        glyphs.append({
            "codepoint": ord(character),
            "uv": [x0 / width, y0 / height, 5 / width, 7 / height],
            "size": [5, 7], "bearing": [0, 0], "advance": 6,
        })
    write_png(directory / "font.png", width, height, pixels)
    (directory / "font.json").write_text(
        json.dumps({"schema_version": "1", "line_height": 9, "glyphs": glyphs}, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )


def make_sprites(directory: Path) -> None:
    width, height = 64, 16
    pixels = bytearray(width * height * 4)
    colors = ((71, 209, 126, 255), (42, 167, 94, 255), (255, 100, 112, 255), (255, 207, 82, 255))
    for tile, color in enumerate(colors):
        for y in range(1, 15):
            for x in range(tile * 16 + 1, tile * 16 + 15):
                set_pixel(pixels, width, x, y, color)
    write_png(directory / "sprites.png", width, height, pixels)


def main() -> None:
    for sample in SAMPLES:
        directory = ROOT / "samples" / sample / "assets"
        directory.mkdir(parents=True, exist_ok=True)
        make_sprites(directory)
        make_font(directory)
    print("generated v0.3 procedural sample assets")


if __name__ == "__main__":
    main()
