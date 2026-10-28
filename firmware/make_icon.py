#!/usr/bin/env python3
"""Generate the farm bridge icon: dark rounded square + cyan printer glyph."""
from PIL import Image, ImageDraw

S = 1024
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# dark rounded panel
d.rounded_rectangle([8, 8, S - 8, S - 8], radius=int(S * 0.22), fill=(18, 22, 28, 255))

CYAN = (56, 189, 248, 255)
GREEN = (74, 222, 128, 255)

# printer body
bx0, by0, bx1, by1 = 250, 280, 774, 700
d.rounded_rectangle([bx0, by0, bx1, by1], radius=48, outline=CYAN, width=26)
# paper top
d.rounded_rectangle([bx0 + 90, by0 - 130, bx1 - 90, by0 + 10], radius=24, fill=(18, 22, 28, 255), outline=CYAN, width=22)
# status LED (green = printing)
d.ellipse([bx0 + 56, by1 - 108, bx0 + 132, by1 - 32], fill=GREEN)
# print lines on paper
for i, w in enumerate([200, 150, 175]):
    y = by0 - 80 + i * 34
    d.rounded_rectangle([bx0 + 150, y, bx0 + 150 + w, y + 16], radius=8, fill=CYAN)

# save multi-size ico
img.resize((256, 256), Image.LANCZOS).save(
    r'C:\Users\zzz\WorkBuddy\农场\app.ico', format='ICO',
    sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
img.resize((256, 256), Image.LANCZOS).save(r'C:\Users\zzz\WorkBuddy\农场\app_icon.png')
print('icon saved')
