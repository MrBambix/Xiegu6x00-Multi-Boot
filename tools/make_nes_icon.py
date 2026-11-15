"""Draws x6100mb/assets/nes.png: the NES emulator tile's icon (a pad drawn here, our own)."""
import os

from PIL import Image, ImageDraw, ImageFont

S = 4                                   # drawn 4x, then made smaller: smooth edges
W = 256 * S
im = Image.new("RGB", (W, W))
d = ImageDraw.Draw(im)
for y in range(W):                      # a dark blue backdrop, lighter at the top
    k = y / W
    d.line([(0, y), (W, y)], fill=(int(28 - 14 * k), int(40 - 20 * k), int(70 - 30 * k)))


def box(x0, y0, x1, y1, r, fill):
    d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=r * S, fill=fill)


box(22, 70, 234, 170, 18, (20, 20, 24))          # the pad's shadow edge
box(18, 64, 230, 162, 18, (206, 208, 214))       # its light grey body
box(30, 76, 218, 150, 10, (40, 42, 48))          # the dark face
box(46, 103, 92, 119, 3, (18, 18, 20))           # the cross
box(61, 88, 77, 134, 3, (18, 18, 20))
box(106, 112, 126, 120, 4, (120, 124, 132))      # select, start
box(132, 112, 152, 120, 4, (120, 124, 132))
for cx in (172, 200):                            # the two round buttons
    d.ellipse([(cx - 13) * S, 101 * S, (cx + 13) * S, 127 * S], fill=(214, 40, 48))
try:
    font = ImageFont.truetype("DejaVuSans-Bold.ttf", 54 * S)
except OSError:
    font = ImageFont.load_default()
txt = "NES"
tw = d.textlength(txt, font=font)
d.text(((W - tw) / 2, 176 * S), txt, font=font, fill=(240, 242, 247))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "x6100mb", "assets", "nes.png")
im.resize((256, 256), Image.LANCZOS).save(out, optimize=True)
print(out)
