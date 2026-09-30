"""Regenerate the launcher icon and TV banner (requires Pillow)."""

from math import cos, radians, sin
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "app/src/main/res"
SCALE = 4
SIZE = 432


def box(values):
    return tuple(value * SCALE for value in values)


foreground = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE))
draw = ImageDraw.Draw(foreground)
draw.rounded_rectangle(box((80, 110, 352, 283)), radius=32 * SCALE, fill="#52DCEB")
draw.rounded_rectangle(box((94, 124, 338, 269)), radius=21 * SCALE, fill="#123B50")
draw.rounded_rectangle(box((107, 137, 325, 148)), radius=5 * SCALE, fill="#1D5A6A")

center = (133, 240)
for radius in (49, 88):
    points = [
        (
            round((center[0] + radius * cos(radians(angle))) * SCALE),
            round((center[1] + radius * sin(radians(angle))) * SCALE),
        )
        for angle in range(-90, 1)
    ]
    draw.line(points, fill="#F2FDFF", width=10 * SCALE, joint="curve")
draw.ellipse(box((121, 228, 145, 252)), fill="#F2FDFF")
draw.rounded_rectangle(box((199, 278, 233, 317)), radius=6 * SCALE, fill="#52DCEB")
draw.rounded_rectangle(box((165, 310, 267, 328)), radius=9 * SCALE, fill="#52DCEB")

foreground = foreground.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
(RES / "drawable").mkdir(parents=True, exist_ok=True)
foreground.save(RES / "drawable/ic_launcher_foreground.png")

base = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE))
base_draw = ImageDraw.Draw(base)
base_draw.rounded_rectangle(box((0, 0, SIZE - 1, SIZE - 1)), radius=96 * SCALE, fill="#102B3C")
base_draw.ellipse(box((266, 12, 472, 218)), fill="#174156")
base = base.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
base.alpha_composite(foreground)

for density, pixels in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    directory = RES / f"mipmap-{density}"
    directory.mkdir(parents=True, exist_ok=True)
    base.resize((pixels, pixels), Image.Resampling.LANCZOS).save(directory / "ic_launcher.png")

banner = Image.new("RGB", (640, 360), "#102B3C")
banner_draw = ImageDraw.Draw(banner)
banner_draw.rounded_rectangle((16, 16, 624, 344), radius=38, outline="#2B7890", width=3)
foreground_banner = foreground.resize((288, 288))
banner.paste(foreground_banner, (4, 36), foreground_banner)
font_path = Path("/System/Library/Fonts/Supplemental/Arial.ttf")
font = ImageFont.truetype(str(font_path), 47) if font_path.exists() else ImageFont.load_default()
small_font = ImageFont.truetype(str(font_path), 25) if font_path.exists() else ImageFont.load_default()
banner_draw.text((280, 120), "LAN ScreenCast", fill="#F2FDFF", font=font)
banner_draw.text((283, 194), "TV RECEIVER", fill="#52DCEB", font=small_font)
banner.resize((320, 180), Image.Resampling.LANCZOS).save(RES / "drawable/tv_banner.png")

base.save(ROOT / "design/icon-preview.png")
