#!/usr/bin/env python3
"""Generate the small, redistributable validation assets for samples/breakout."""

from __future__ import annotations

import json
import math
import struct
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "samples" / "breakout" / "assets"


def make_sprites() -> None:
    image = Image.new("RGBA", (256, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((3, 20, 61, 44), radius=10, fill=(76, 201, 240, 255), outline=(198, 245, 255, 255), width=2)
    draw.ellipse((78, 14, 114, 50), fill=(255, 218, 90, 255), outline=(255, 248, 194, 255), width=3)
    draw.rounded_rectangle((131, 10, 189, 54), radius=7, fill=(255, 255, 255, 255), outline=(255, 255, 255, 255), width=2)
    draw.rectangle((194, 0, 255, 63), fill=(255, 255, 255, 255))
    image.save(ASSETS / "sprites.png", optimize=True)


def glyph_characters() -> str:
    visible_text = """
    AI2D 브레이크아웃 시작 설정 종료 프레임 제한 무제한 뒤로 점수 목숨 승리 메뉴로 게임 오버
    이동 또는 공 발사 다시 플레이 FPS 0123456789:-/←→ADSpaceEnterEscTab!
    """
    return "".join(sorted(set(visible_text.replace("\n", ""))))


def make_font() -> None:
    font_path = Path(r"C:\Windows\Fonts\malgun.ttf")
    if not font_path.exists():
        raise RuntimeError(f"Required system font was not found: {font_path}")
    font = ImageFont.truetype(str(font_path), 34)
    characters = glyph_characters()
    cell_width = 64
    cell_height = 64
    columns = 16
    rows = math.ceil(len(characters) / columns)
    atlas = Image.new("RGBA", (columns * cell_width, rows * cell_height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(atlas)
    glyphs: list[dict[str, object]] = []
    for index, character in enumerate(characters):
        column = index % columns
        row = index // columns
        x = column * cell_width
        y = row * cell_height
        bbox = font.getbbox(character)
        width = max(1, bbox[2] - bbox[0])
        height = max(1, bbox[3] - bbox[1])
        draw.text((x + 3 - bbox[0], y + 3 - bbox[1]), character, font=font, fill=(255, 255, 255, 255))
        glyphs.append(
            {
                "codepoint": ord(character),
                "uv": [
                    (x + 3) / atlas.width,
                    (y + 3) / atlas.height,
                    width / atlas.width,
                    height / atlas.height,
                ],
                "size": [width, height],
                "bearing": [0, 0],
                "advance": float(font.getlength(character)),
            }
        )
    atlas.save(ASSETS / "font.png", optimize=True)
    metadata = {"schema_version": "1", "line_height": 48, "glyphs": glyphs}
    (ASSETS / "font.json").write_text(
        json.dumps(metadata, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )


def make_tone(name: str, frequency: float, duration: float, volume: float) -> None:
    sample_rate = 48_000
    frame_count = int(sample_rate * duration)
    path = ASSETS / name
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        frames = bytearray()
        for index in range(frame_count):
            envelope = 1.0 - index / frame_count
            sample = math.sin(2.0 * math.pi * frequency * index / sample_rate) * volume * envelope
            frames.extend(struct.pack("<h", int(max(-1.0, min(1.0, sample)) * 32767)))
        output.writeframes(frames)


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    make_sprites()
    make_font()
    make_tone("hit.wav", 720.0, 0.07, 0.25)
    make_tone("lose.wav", 180.0, 0.22, 0.32)
    print(f"generated breakout assets in {ASSETS}")


if __name__ == "__main__":
    main()
