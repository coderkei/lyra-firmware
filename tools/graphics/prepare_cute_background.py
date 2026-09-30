#!/usr/bin/env python3
"""Prepare the opaque Cute & Pink asset; this does not build firmware."""

from pathlib import Path

from PIL import Image


def main() -> None:
    graphics = Path(__file__).resolve().parents[2] / "graphics"
    with Image.open(graphics / "cute_pink_background_source.png") as source:
        if source.width * 3 != source.height * 2:
            raise ValueError("Cute background source must have a 2:3 portrait aspect ratio")
        image = source.convert("RGB").resize((320, 480), Image.Resampling.LANCZOS)
    # Bake a pale tint for legible indigo text without runtime alpha blending.
    image = Image.blend(Image.new("RGB", image.size, (255, 245, 252)), image, 0.78)
    image = image.quantize(colors=128, dither=Image.Dither.NONE)
    target = graphics / "cute_pink_background.png"
    image.save(target, optimize=True)
    print(f"{target.name}: 320x480, 128 colours, {target.stat().st_size:,} bytes")


if __name__ == "__main__":
    main()
