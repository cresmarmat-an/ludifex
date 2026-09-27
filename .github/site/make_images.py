"""Writes the site's images from the library's icon.

    python .github/site/make_images.py

The library's icon is Icon.png at the repository's root, and sibling.png next
to this script is the other library's icon, copied unchanged from its
repository. This crops each one to its
artwork and scales it for the page, the favicons, and the social preview card
(assets/img/og.png). Run it again when an icon changes. The results are
committed, so building the site does not need Pillow.
"""

import json
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("make_images.py needs Pillow: pip install pillow")

SITE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(SITE))
OUT = os.path.join(SITE, "assets", "img")
BG = (224, 229, 236, 255)  # the light theme's background
TEXT = (35, 39, 47)
MUTED = (91, 100, 114)


def artwork(path):
    """The icon cropped to a square around its artwork, ignoring stray specks."""
    image = Image.open(path).convert("RGBA")
    alpha = image.split()[3].point(lambda a: 255 if a > 128 else 0)
    width, height = image.size
    pixels = alpha.load()
    rows = [y for y in range(height) if sum(1 for x in range(0, width, 2) if pixels[x, y]) > 3]
    cols = [x for x in range(width) if sum(1 for y in range(0, height, 2) if pixels[x, y]) > 3]
    left, right, top, bottom = cols[0], cols[-1] + 1, rows[0], rows[-1] + 1
    side = max(right - left, bottom - top)
    cx, cy = (left + right) / 2, (top + bottom) / 2
    half = side / 2
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    box = (round(cx - half), round(cy - half), round(cx + half), round(cy + half))
    square.paste(image.crop(box), (0, 0))
    return square


def scaled(art, size, fill=1.0, background=None):
    """The artwork at `size`, taking up `fill` of it, centred."""
    inner = max(1, round(size * fill))
    canvas = Image.new("RGBA", (size, size), background or (0, 0, 0, 0))
    canvas.alpha_composite(art.resize((inner, inner), Image.LANCZOS), ((size - inner) // 2, (size - inner) // 2))
    return canvas


def font(names, size):
    for name in names:
        for folder in (os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts"), "/usr/share/fonts/truetype/dejavu"):
            path = os.path.join(folder, name)
            if os.path.isfile(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default(size)


def social_card(art, config):
    width, height = 1200, 630
    card = Image.new("RGBA", (width, height), BG)
    size, x = 300, 110
    card.alpha_composite(scaled(art, size), (x, (height - size) // 2))

    draw = ImageDraw.Draw(card)
    left = x + size + 90
    draw.text((left, 190), config["name"], font=font(["segoeuib.ttf", "DejaVuSans-Bold.ttf"], 118), fill=TEXT)
    words, lines, line = config["tagline"].split(), [], ""
    body = font(["seguisb.ttf", "DejaVuSans.ttf"], 36)
    for word in words:
        trial = (line + " " + word).strip()
        if draw.textlength(trial, font=body) > width - left - 70:
            lines.append(line)
            line = word
        else:
            line = trial
    lines.append(line)
    for i, text in enumerate(lines):
        draw.text((left, 350 + i * 48), text, font=body, fill=MUTED)
    return card.convert("RGB")


def main():
    config = json.load(open(os.path.join(SITE, "config.json"), encoding="utf-8"))
    os.makedirs(OUT, exist_ok=True)
    art = artwork(os.path.join(REPO, "Icon.png"))
    scaled(art, 96, 0.9).save(os.path.join(OUT, "icon-96.png"), optimize=True)
    scaled(art, 512, 0.9).save(os.path.join(OUT, "icon-512.png"), optimize=True)
    scaled(art, 32).save(os.path.join(OUT, "icon-32.png"), optimize=True)
    scaled(art, 192, 0.94).save(os.path.join(OUT, "icon-192.png"), optimize=True)
    # Apple's home-screen icon has to be opaque, so it takes the page's background.
    scaled(art, 180, 0.8, BG).convert("RGB").save(os.path.join(OUT, "icon-180.png"), optimize=True)
    scaled(artwork(os.path.join(SITE, "sibling.png")), 96, 0.9).save(os.path.join(OUT, "sibling-96.png"), optimize=True)
    social_card(art, config).save(os.path.join(OUT, "og.png"), optimize=True)
    print("images written to", OUT)


if __name__ == "__main__":
    main()
