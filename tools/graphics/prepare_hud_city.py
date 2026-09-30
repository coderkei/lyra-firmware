#!/usr/bin/env python3
"""Prepare the embedded HUD PNG; this does not build firmware.

Uses the existing EMBED_FILES + decode_png pipeline. The source is retained
for future asset work; only the 320x480 indexed PNG is linked into firmware.
"""

from pathlib import Path

from PIL import Image


def main() -> None:
    graphics = Path(__file__).resolve().parents[2] / "graphics"
    source = graphics / "neon_hud_city_source.png"
    target = graphics / "neon_hud_city.png"
    with Image.open(source) as image:
        # The generated source is already portrait 2:3. Avoid stretching or
        # cropping a replacement with a different aspect ratio silently.
        if image.width * 3 != image.height * 2:
            raise ValueError("HUD city source must have a 2:3 portrait aspect ratio")
        prepared = image.convert("RGB").resize((320, 480), Image.Resampling.LANCZOS)
        prepared = prepared.quantize(colors=128, dither=Image.Dither.NONE)
        prepared.save(target, optimize=True)
    print(f"{target.name}: 320x480, 128 colours, {target.stat().st_size:,} bytes")


if __name__ == "__main__":
    main()
