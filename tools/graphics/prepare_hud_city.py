#!/usr/bin/env python3
"""Prepare opaque Neon Sky PNGs; this does not build firmware.

Uses the existing EMBED_FILES + decode_png pipeline. The source is retained
for future asset work; only the two 320x480 indexed PNGs are embedded.
"""

from pathlib import Path

from PIL import Image, ImageDraw


def decorate(image: Image.Image) -> None:
    """Bake simple HUD geometry once, rather than drawing it while scrolling."""
    draw = ImageDraw.Draw(image)
    for x in range(12, 320, 32):
        draw.line((x, 28, x, 479), fill=(10, 27, 36), width=1)
    for y in range(40, 480, 32):
        draw.line((0, y, 319, y), fill=(38, 9, 22), width=1)
    draw.line((3, 76, 3, 478), fill=(83, 16, 39), width=1)
    draw.line((316, 76, 316, 478), fill=(83, 16, 39), width=1)
    # Both dock heights are covered naturally by the opaque navigation dock.
    for bottom in (434, 478):
        draw.line((3, bottom - 10, 13, bottom, 37, bottom), fill=(160, 19, 55), width=1)
        draw.line((282, bottom, 306, bottom, 316, bottom - 10), fill=(160, 19, 55), width=1)
    draw.line((295, 83, 307, 83, 316, 92), fill=(83, 16, 39), width=1)
    for y in range(96, 466, 48):
        draw.line((3, y, 7, y), fill=(160, 19, 55), width=1)


def main() -> None:
    graphics = Path(__file__).resolve().parents[2] / "graphics"
    source = graphics / "neon_sky_city_source.png"
    with Image.open(source) as image:
        # The generated source is already portrait 2:3. Avoid stretching or
        # cropping a replacement with a different aspect ratio silently.
        if image.width * 3 != image.height * 2:
            raise ValueError("HUD city source must have a 2:3 portrait aspect ratio")
        resized = image.convert("RGB").resize((320, 480), Image.Resampling.LANCZOS)
    background = Image.new("RGB", resized.size, (5, 8, 15))
    for filename, visibility in (("neon_sky_city.png", 0.85),
                                  ("neon_sky_city_lists.png", 0.38)):
        # All tinting/dimming is baked into RGB pixels. LVGL copies these
        # opaque RGB565 images without image or per-row alpha blending.
        prepared = Image.blend(background, resized, visibility)
        decorate(prepared)
        prepared = prepared.quantize(colors=128, dither=Image.Dither.NONE)
        target = graphics / filename
        prepared.save(target, optimize=True)
        print(f"{target.name}: 320x480, 128 colours, {target.stat().st_size:,} bytes")


if __name__ == "__main__":
    main()
