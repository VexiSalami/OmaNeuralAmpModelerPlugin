"""Generates Korpi's background images (Background.jpg, @2x, @3x).

Dark spruce forest under a faint aurora. Run with:
    python make_korpi_background.py <output_dir>
"""

import math
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

W, H = 760, 430  # Must match PLUG_WIDTH / PLUG_HEIGHT in config.h
S = 3  # Draw at 3x, then scale down


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def spruce(draw, x, base_y, height, color, rng):
    """Simple spruce silhouette made of stacked, slightly ragged tiers."""
    width = height * rng.uniform(0.28, 0.36)
    tiers = max(5, int(height / (14 * S)))
    for i in range(tiers):
        t0 = i / tiers
        top = base_y - height + height * t0 * 0.92
        bottom = top + height / tiers * 2.1
        half = width * (0.12 + 0.88 * (t0 + 1.0 / tiers)) / 2
        jag = half * rng.uniform(-0.08, 0.08)
        draw.polygon([(x, top), (x - half - jag, bottom), (x + half + jag, bottom)], fill=color)
    draw.rectangle([x - width * 0.04, base_y - height * 0.1, x + width * 0.04, base_y + 4 * S], fill=color)
    draw.line([(x, base_y - height - 6 * S), (x, base_y - height + 10 * S)], fill=color, width=max(1, S))


def main(out_dir):
    rng = random.Random(7)
    w, h = W * S, H * S

    # Sky gradient: very dark teal at top -> deep green near horizon -> almost black ground
    img = Image.new("RGB", (w, h))
    px = img.load()
    top, mid, bottom = (8, 22, 22), (14, 48, 40), (6, 16, 14)
    for y in range(h):
        t = y / h
        c = lerp(top, mid, t / 0.62) if t < 0.62 else lerp(mid, bottom, (t - 0.62) / 0.38)
        for x in range(w):
            px[x, y] = c

    # Aurora: soft diagonal bands of light green, heavily blurred
    aurora = Image.new("L", (w, h), 0)
    ad = ImageDraw.Draw(aurora)
    for k in range(5):
        cx = w * (0.25 + 0.14 * k) + rng.uniform(-40, 40) * S
        amp = rng.uniform(25, 45) * S
        for yy in range(0, int(h * 0.55), 2):
            xx = cx + math.sin(yy / (60 * S) + k) * amp - yy * 0.35
            fade = 1.0 - yy / (h * 0.55)
            ad.ellipse([xx - 18 * S, yy - 3 * S, xx + 18 * S, yy + 3 * S], fill=int(70 * fade))
    aurora = aurora.filter(ImageFilter.GaussianBlur(28 * S))
    glow = Image.new("RGB", (w, h), (70, 190, 140))
    img = Image.composite(glow, img, aurora)

    # Stars, very faint
    d = ImageDraw.Draw(img)
    for _ in range(90):
        x, y = rng.uniform(0, w), rng.uniform(0, h * 0.4)
        b = rng.randint(70, 140)
        d.ellipse([x, y, x + S * 0.8, y + S * 0.8], fill=(b, b + 20, b + 10))

    # Distant fell (tunturi) and ridges
    ridge = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ridge)
    pts = [(0, h)]
    for x in range(0, w + 1, 6 * S):
        y = h * 0.66 - 55 * S * math.exp(-(((x - w * 0.5) / (w * 0.18)) ** 2)) + math.sin(x / (90 * S)) * 6 * S
        pts.append((x, y))
    pts.append((w, h))
    rd.polygon(pts, fill=(12, 38, 33, 255))
    img.paste(ridge, (0, 0), ridge)

    # Mist layer
    mist = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(mist)
    md.rectangle([0, h * 0.68, w, h * 0.80], fill=60)
    mist = mist.filter(ImageFilter.GaussianBlur(20 * S))
    img = Image.composite(Image.new("RGB", (w, h), (30, 70, 58)), img, mist)

    # Forest layers: far (lighter), then near (darker); denser at the sides
    for layer, (color, base, hmin, hmax, count) in enumerate(
        [
            ((10, 32, 28), h * 0.84, 40, 80, 70),
            ((6, 20, 18), h * 0.98, 70, 150, 34),
        ]
    ):
        trees = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        td = ImageDraw.Draw(trees)
        for _ in range(count):
            # Bias trees toward the left and right edges so the controls stay clear
            side = rng.random()
            x = rng.uniform(0, w * 0.22) if side < 0.4 else rng.uniform(w * 0.78, w) if side < 0.8 else rng.uniform(0, w)
            hh = rng.uniform(hmin, hmax) * S * (1.3 if (x < w * 0.12 or x > w * 0.88) else 1.0)
            spruce(td, x, base + rng.uniform(-6, 6) * S, hh, color + (255,), rng)
        img.paste(trees, (0, 0), trees)

    # Vignette for depth
    vig = Image.new("L", (w, h), 0)
    vd = ImageDraw.Draw(vig)
    vd.rectangle([0, 0, w, h], fill=150)
    vd.rounded_rectangle([40 * S, 30 * S, w - 40 * S, h - 30 * S], radius=60 * S, fill=0)
    vig = vig.filter(ImageFilter.GaussianBlur(45 * S))
    img = Image.composite(Image.new("RGB", (w, h), (2, 6, 5)), img, vig)

    # Fine grain, like the reference image
    noise = Image.effect_noise((w, h), 10).convert("L")
    img = Image.blend(img, Image.merge("RGB", (noise, noise, noise)), 0.035)

    # Faint panel edge so the UI reads as a device, not just a picture
    pd = ImageDraw.Draw(img)
    pd.rounded_rectangle([20 * S, 20 * S, w - 20 * S, h - 20 * S], radius=14 * S, outline=(60, 120, 100), width=S)

    for scale, suffix in [(1, ""), (2, "@2x"), (3, "@3x")]:
        out = img.resize((W * scale, H * scale), Image.LANCZOS)
        out.save(f"{out_dir}/Background{suffix}.jpg", quality=92)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
