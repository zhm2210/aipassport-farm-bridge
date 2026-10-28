#!/usr/bin/env python3
"""
sim.py — 用固件里同一份点阵字库+同一套坐标，把界面渲染成 PNG
目的：不依赖实机，就能看出是字库坏了还是绘制逻辑坏了
"""
import re
from PIL import Image

HDR = r"C:\Users\zzz\WorkBuddy\农场\firmware\aipassport_farm\cnfont.h"
OUT = r"C:\Users\zzz\WorkBuddy\农场\.tmp\screen_sim.png"

# 颜色（与固件一致，RGB565 → RGB888）
def c565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)

C_BG      = c565(0x0863)
C_CARD    = c565(0x18C4)
C_TEXT    = c565(0xE71C)
C_DIM     = c565(0x8C71)
C_ACCENT  = c565(0x5D9F)
C_OK      = c565(0x2E58)
C_WARN    = c565(0xFD20)
C_LINE    = c565(0x39E8)

src = open(HDR, encoding='utf-8').read()
m = re.search(r'static const uint8_t CN16_BITS\[\] = \{(.*?)\};', src, re.S)
BITS = [int(x) for x in re.findall(r'\d+', m.group(1))]
m2 = re.search(r'static const CnGlyph CN16_MAP\[\] = \{(.*?)\};', src, re.S)
MAP = {}
for cp, off, w, h in re.findall(r'\{(\d+),(\d+),(\d+),(\d+)\}', m2.group(1)):
    MAP[int(cp)] = (int(off), int(w), int(h))

print(f"字库: {len(BITS)} 字节, {len(MAP)} 字形")
missing = []

def glyph_cp(cp):
    """返回 (w,h,rows) rows 是 [[bool]]"""
    if cp not in MAP:
        missing.append(cp)
        return 0, 0, []
    off, w, h = MAP[cp]
    stride = (w + 7) // 8
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            byte = BITS[off + y * stride + x // 8]
            row.append(bool(byte & (0x80 >> (x % 8))))
        rows.append(row)
    return w, h, rows

img = Image.new('RGB', (240, 320), C_BG)
px = img.load()

def width(s):
    w = 0
    for ch in s:
        gw, gh, _ = glyph_cp(ord(ch))
        w += gw
    return w

def draw(x, y, s, fg, bg):
    cx = x
    for ch in s:
        gw, gh, rows = glyph_cp(ord(ch))
        if not gw:
            cx += 8
            continue
        # 背景
        for j in range(gh):
            for i in range(gw):
                if 0 <= cx + i < 240 and 0 <= y + j < 320:
                    px[cx + i, y + j] = bg
        for j in range(gh):
            for i in range(gw):
                if rows[j][i] and 0 <= cx + i < 240 and 0 <= y + j < 320:
                    px[cx + i, y + j] = fg
        cx += gw

def draw_c(s, y, fg, bg):
    draw(120 - width(s) // 2, y, s, fg, bg)

def fill(x, y, w, h, c):
    for j in range(h):
        for i in range(w):
            if 0 <= x + i < 240 and 0 <= y + j < 320:
                px[x + i, y + j] = c

def hline(x, y, w, c):
    for i in range(w):
        if 0 <= x + i < 240: px[x + i, y] = c

# ---------- 模拟首页 ----------
fill(0, 0, 240, 28, C_CARD)
draw(8, 6, "拓竹农场", C_TEXT, C_CARD)
draw(232 - width("wesley"), 6, "wesley", C_DIM, C_CARD)
hline(0, 28, 240, C_LINE)

def draw_scaled(cx, y, s, fg, bg, k):
    """居中等比放大绘制（模拟 TFT_eSPI 大字体）"""
    total = sum(glyph_cp(ord(c))[0] for c in s) * k if s else 0
    x = cx - total // 2
    for ch in s:
        gw, gh, rows = glyph_cp(ord(ch))
        if not gw:
            x += 8 * k
            continue
        for j in range(gh * k):
            for i in range(gw * k):
                if 0 <= x + i < 240 and 0 <= y + j < 320:
                    px[x + i, y + j] = fg if rows[j // k][i // k] else bg
        x += gw * k

cards = [("打印中", 3, C_OK), ("空闲", 30, C_DIM), ("队列", 7, C_ACCENT)]
cw, ch, y0 = 70, 56, 34
for i, (lab, val, col) in enumerate(cards):
    x = 10 + i * (cw + 7)
    fill(x, y0, cw, ch, C_CARD)
    # 26px ≈ 16px 字形放大 1.6 倍
    draw_scaled(x + cw // 2, y0 + 6, str(val), col, C_CARD, 2)
    draw(x + cw // 2 - width(lab) // 2, y0 + 36, lab, C_DIM, C_CARD)

draw(10, 114, "正在打印", C_DIM, C_BG)
draw(230 - width("3/33台"), 114, "3/33台", C_DIM, C_BG)
draw(14, 134, "A1-01", C_TEXT, C_CARD)
fill(8, 134 - 3, 224, 34, C_CARD)
draw(14, 134 - 3 + 3, "A1-01", C_TEXT, C_CARD)
draw(226 - width("88% 42分"), 134, "88% 42分", C_TEXT, C_CARD)

draw(10, 210, "最近事件", C_DIM, C_BG)
draw(10, 228, "A1-03 暂停 HMS_0300", C_WARN, C_BG)

tabs = ["总览", "设备", "任务", "事件"]
for i, t in enumerate(tabs):
    x = i * 60
    act = (i == 0)
    fill(x, 272, 60, 22, C_CARD if act else C_BG)
    draw(x + 30 - width(t) // 2, 275, t, C_ACCENT if act else C_DIM, C_CARD if act else C_BG)
    if act: hline(x + 8, 291, 44, C_ACCENT)

fill(0, 296, 240, 24, C_CARD)
hline(0, 296, 240, C_LINE)
draw_c("上下切页  OK 进入", 299, C_DIM, C_CARD)

img.save(OUT)
print("saved", OUT)
if missing:
    print("缺失字形:", [hex(c) for c in missing[:20]], "共", len(missing))
