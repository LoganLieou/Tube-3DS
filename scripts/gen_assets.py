#!/usr/bin/env python3
"""Generates Tube3DS's icon, CIA banner image and banner sound into assets/.

The outputs are checked in; rerun only to change the artwork. Needs Pillow.
"""
import math
import os
import struct
import wave

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ASSETS = os.path.join(ROOT, "assets")

RED = (255, 0, 51)
DARK = (15, 15, 15)
WHITE = (255, 255, 255)
GREY = (170, 170, 170)

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def play_button(draw, cx, cy, w):
    """A red rounded 'play' pill with a white triangle, centered on (cx, cy)."""
    h = w * 0.7
    r = h * 0.28
    draw.rounded_rectangle([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], radius=r, fill=RED)
    t = h * 0.42
    draw.polygon(
        [(cx - t * 0.45, cy - t * 0.55), (cx - t * 0.45, cy + t * 0.55), (cx + t * 0.6, cy)],
        fill=WHITE,
    )


def make_icon():
    s = 4  # supersample, then downscale for smooth edges
    img = Image.new("RGB", (48 * s, 48 * s), DARK)
    d = ImageDraw.Draw(img)
    # Subtle vertical gradient background.
    for y in range(48 * s):
        v = int(30 - 18 * y / (48 * s))
        d.line([(0, y), (48 * s, y)], fill=(v, v, v))
    play_button(d, 24 * s, 21 * s, 34 * s)
    f = font(9 * s)
    d.text((24 * s, 40 * s), "3DS", font=f, fill=WHITE, anchor="mm")
    img.resize((48, 48), Image.Resampling.LANCZOS).save(os.path.join(ASSETS, "icon.png"))


def make_banner():
    # The home menu shows the banner at 256x128; keep content in the middle.
    s = 2
    W, H = 256 * s, 128 * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8 * s, 18 * s, W - 8 * s, H - 18 * s], radius=14 * s, fill=(24, 24, 24, 255))
    play_button(d, 58 * s, 64 * s, 62 * s)
    d.text((100 * s, 56 * s), "Tube3DS", font=font(30 * s), fill=WHITE, anchor="lm")
    d.text((101 * s, 82 * s), "YouTube for your 3DS", font=font(12 * s), fill=GREY, anchor="lm")
    img.resize((256, 128), Image.Resampling.LANCZOS).save(os.path.join(ASSETS, "banner.png"))


def make_sound():
    # Two soft notes, ~1.2 s, 16-bit mono.
    rate = 32000
    notes = [(659.25, 0.0, 0.5), (987.77, 0.18, 1.0)]  # E5 then B5
    n = int(rate * 1.2)
    samples = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for freq, start, length in notes:
            if start <= t < start + length:
                k = t - start
                env = min(1.0, k / 0.01) * math.exp(-k * 4.0)
                v += math.sin(2 * math.pi * freq * k) * env * 0.35
                v += math.sin(4 * math.pi * freq * k) * env * 0.08
        samples.append(int(max(-1.0, min(1.0, v)) * 32767))
    with wave.open(os.path.join(ASSETS, "banner.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


if __name__ == "__main__":
    os.makedirs(ASSETS, exist_ok=True)
    make_icon()
    make_banner()
    make_sound()
    print("wrote assets/icon.png, assets/banner.png, assets/banner.wav")
