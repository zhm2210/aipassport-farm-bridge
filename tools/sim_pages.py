#!/usr/bin/env python3
"""
sim_pages.py — 把固件的各个页面按同一套坐标渲染成 PNG，检查布局重叠
"""
import re
from PIL import Image

HDR = r"C:\Users\zzz\WorkBuddy\农场\firmware\aipassport_farm\cnfont.h"
OUTDIR = r"C:\Users\zzz\WorkBuddy\农场\.tmp"

def c565(v):
    return (((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31)

C_BG     = c565(0x0863)
C_CARD   = c565(0x18C4)
C_CARD2  = c565(0x20E6)
C_TEXT   = c565(0xE71C)
C_DIM    = c565(0x8C71)
C_ACCENT = c565(0x5D9F)
C_OK     = c565(0x2E58)
C_WARN   = c565(0xFD20)
C_ERR    = c565(0xF1C7)
C_LINE   = c565(0x39E8)

src = open(HDR, encoding='utf-8').read()
BITS = [int(x) for x in re.findall(r'\d+', re.search(r'CN16_BITS\[\] = \{(.*?)\};', src, re.S).group(1))]
MAP = {}
for cp, off, w, h in re.findall(r'\{(\d+),(\d+),(\d+),(\d+)\}',
                                re.search(r'CN16_MAP\[\] = \{(.*?)\};', src, re.S).group(1)):
    MAP[int(cp)] = (int(off), int(w), int(h))

class Screen:
    def __init__(self):
        self.img = Image.new('RGB', (240, 320), C_BG)
        self.px = self.img.load()
    def fill(self, x, y, w, h, c):
        for j in range(max(0, y), min(320, y + h)):
            for i in range(max(0, x), min(240, x + w)):
                self.px[i, j] = c
    def round_rect(self, x, y, w, h, r, c):
        # 简化：直角
        self.fill(x, y, w, h, c)
    def hline(self, x, y, w, c):
        for i in range(w):
            if 0 <= x + i < 240 and 0 <= y < 320:
                self.px[x + i, y] = c
    def glyph_w(self, ch):
        g = MAP.get(ord(ch))
        return g[1] if g else 8
    def width(self, s):
        return sum(self.glyph_w(c) for c in s)
    def text(self, x, y, s, fg, bg):
        cx = x
        for ch in s:
            g = MAP.get(ord(ch))
            if not g:
                cx += 8
                continue
            off, gw, gh = g
            stride = (gw + 7) // 8
            self.fill(cx, y, gw, gh, bg)
            for j in range(gh):
                for i in range(gw):
                    if BITS[off + j * stride + i // 8] & (0x80 >> (i % 8)):
                        if 0 <= cx + i < 240 and 0 <= y + j < 320:
                            self.px[cx + i, y + j] = fg
            cx += gw
    def ctext(self, cx, y, s, fg, bg):
        self.text(cx - self.width(s) // 2, y, s, fg, bg)
    def rtext(self, rx, y, s, fg, bg):
        self.text(rx - self.width(s), y, s, fg, bg)
    def big(self, cx, y, s, fg, bg, k=2):
        total = sum(self.glyph_w(c) for c in s) * k
        x = cx - total // 2
        for ch in s:
            g = MAP.get(ord(ch))
            if not g:
                x += 8 * k
                continue
            off, gw, gh = g
            stride = (gw + 7) // 8
            self.fill(x, y, gw * k, gh * k, bg)
            for j in range(gh * k):
                for i in range(gw * k):
                    if BITS[off + (j // k) * stride + (i // k) // 8] & (0x80 >> ((i // k) % 8)):
                        if 0 <= x + i < 240 and 0 <= y + j < 320:
                            self.px[x + i, y + j] = fg
            x += gw * k
    def bar(self, x, y, w, h, pct, col):
        self.fill(x, y, w, h, C_DIM)
        f = (w - 4) * max(0, min(100, pct)) // 100
        if f > 0:
            self.fill(x + 2, y + 2, f, h - 4, col)

def header(s, title, right=None):
    s.fill(0, 0, 240, 28, C_CARD)
    s.text(8, 6, title, C_TEXT, C_CARD)
    s.rtext(232, 6, right or "wesley", C_DIM, C_CARD)
    s.hline(0, 28, 240, C_LINE)

def footer(s, hint):
    s.fill(0, 296, 240, 24, C_CARD)
    s.hline(0, 296, 240, C_LINE)
    s.ctext(120, 299, hint, C_DIM, C_CARD)

DEV = [("A1-01", "RUNNING", 69, 42), ("A1-02", "IDLE", 0, 0), ("A1-03", "RUNNING", 94, 8),
       ("A1-04", "IDLE", 0, 0), ("A1-c1", "PAUSE", 76, 30), ("A1-c3", "RUNNING", 63, 55),
       ("A1-c7", "IDLE", 0, 0), ("A1-b2", "FAILED", 12, 0)]
ST = {"RUNNING": "打印中", "IDLE": "空闲", "PAUSE": "已暂停", "FAILED": "打印失败", "FINISH": "已完成"}
SC = {"RUNNING": C_OK, "IDLE": C_DIM, "PAUSE": C_WARN, "FAILED": C_ERR, "FINISH": C_DIM}

# ---------- 1. 任务列表 ----------
s = Screen()
header(s, "任务队列 7个")
TASKS = [("1_plate_11", 2, 1), ("1_plate_4", 10, 0), ("1_plate_7", 10, 0),
         ("1_plate_5", 15, 0), ("1_plate_8", 13, 0), ("1_plate_6", 5, 0), ("1_plate_9", 5, 0)]
rows = [("‹ 返回总览", None)] + TASKS
for r, item in enumerate(rows[:7]):
    y = 30 + r * 36
    sel = (r == 3)
    if sel: s.fill(4, y, 232, 34, C_CARD)
    if item[1] is None:
        s.text(12, y + 9, item[0], C_ACCENT, sel and C_CARD or C_BG)
        continue
    name, rem, wk = item
    s.text(10, y + 2, name, C_TEXT, sel and C_CARD or C_BG)
    s.text(10, y + 20, f"剩{rem}件  已派{wk}台", C_DIM, sel and C_CARD or C_BG)
    if sel: s.rtext(228, y + 10, "OK 下发", C_ACCENT, C_CARD)
footer(s, "上下移动  OK 选择下发")
s.img.save(f"{OUTDIR}/page_tasks.png")

# ---------- 2. 选机台（单行布局）----------
s = Screen()
header(s, "选机台 已选2台", "1_plate_5")
rows = [("‹ 返回任务", None)] + [(d[0], d) for d in DEV]
for r, (label, d) in enumerate(rows[:7]):
    y = 30 + r * 32
    sel = (r == 2)
    if sel: s.fill(4, y, 232, 30, C_CARD)
    if d is None:
        s.text(12, y + 7, label, C_ACCENT, sel and C_CARD or C_BG); continue
    name, st, prog, rem = d
    ok = st in ("IDLE", "FINISH")
    pick = name in ("A1-02", "A1-04")
    bg = sel and C_CARD or C_BG
    s.fill(10, y + 10, 12, 12, C_DIM)
    s.fill(11, y + 11, 10, 10, bg)
    if pick: s.fill(12, y + 12, 8, 8, C_OK)
    s.text(28, y + 7, name, (C_OK if pick else C_TEXT) if ok else C_DIM, bg)
    s.text(104, y + 7, ST[st], SC[st] if ok else C_DIM, bg)
    if st == "RUNNING":
        s.rtext(228, y + 7, f"{prog}%", C_DIM, bg)
    else:
        s.rtext(228, y + 7, "可勾选" if ok else "使用中", C_DIM if ok else C_WARN, bg)
footer(s, "OK 勾选 长按返回")
s.img.save(f"{OUTDIR}/page_select.png")

# ---------- 2b. 设备列表（rowH 36）----------
s = Screen()
header(s, "设备 33台")
rows = [("‹ 返回总览", None)] + [(d[0], d) for d in DEV]
for r, (label, d) in enumerate(rows[:7]):
    y = 30 + r * 36
    sel = (r == 2)
    if sel: s.fill(4, y, 232, 34, C_CARD)
    if d is None:
        s.text(12, y + 9, label, C_ACCENT, sel and C_CARD or C_BG); continue
    name, st, prog, rem = d
    bg = sel and C_CARD or C_BG
    s.text(10, y + 2, name, C_TEXT, bg)
    s.text(10, y + 20, ST[st], SC[st], bg)
    if st == "RUNNING":
        s.bar(96, y + 22, 66, 7, prog, C_OK)
        s.rtext(228, y + 2, f"{prog}%", C_TEXT, bg)
        s.rtext(228, y + 20, f"{rem}分", C_DIM, bg)
    elif st == "PAUSE":
        s.rtext(228, y + 10, "等待恢复", C_WARN, bg)
footer(s, "上下移动  OK 详情")
s.img.save(f"{OUTDIR}/page_devlist.png")

# ---------- 3. 下发选项 ----------
s = Screen()
header(s, "下发选项")
s.text(10, 36, "1_plate_5", C_TEXT, C_BG)
s.text(10, 56, "剩15件  本次派2台", C_DIM, C_BG)
y = 84
# 行1
s.fill(6, y, 228, 36, C_CARD)
s.text(14, y + 4, "料盘映射", C_DIM, C_CARD)
s.text(14, y + 21, "外挂料盘", C_ACCENT, C_CARD)
s.rtext(226, y + 12, "OK 切换", C_ACCENT, C_CARD)
y += 40
s.text(14, y + 4, "已选机台", C_DIM, C_BG)
s.text(14, y + 21, "A1-02 A1-04", C_TEXT, C_BG)
y += 40
s.fill(6, y, 228, 38, C_CARD)
s.ctext(120, y + 11, "确认下发", C_OK, C_CARD)
s.text(10, 238, "料盘映射决定实际用哪卷材料", C_DIM, C_BG)
s.text(10, 256, "选错会打错料，请先核对机台", C_DIM, C_BG)
footer(s, "上下选择  OK 操作")
s.img.save(f"{OUTDIR}/page_options.png")

# ---------- 4. 设备详情 ----------
s = Screen()
header(s, "A1-01")
s.text(10, 36, "1_plate_7", C_TEXT, C_BG)
s.fill(10, 58, 88, 22, C_CARD)
s.text(18, 61, "打印中", C_OK, C_CARD)
s.bar(10, 88, 220, 10, 69, C_OK)
s.text(10, 102, "69%", C_DIM, C_BG)
s.rtext(230, 102, "剩余42分", C_DIM, C_BG)
s.fill(8, 122, 224, 40, C_CARD)
s.text(14, 132, "喷嘴 239℃", C_TEXT, C_CARD)
s.text(126, 132, "热床 74℃", C_TEXT, C_CARD)
s.fill(8, 176, 224, 34, C_CARD)
s.ctext(120, 185, "暂停打印", C_WARN, C_CARD)
s.ctext(120, 227, "‹ 返回设备列表", C_ACCENT, C_BG)
footer(s, "上下选择  OK 执行")
s.img.save(f"{OUTDIR}/page_devaction.png")

print("saved 4 page pngs")

# ---------- 5. 设置 ----------
s = Screen()
header(s, "设置")
rows = [52, 100, 156]
for i, y in enumerate(rows):
    sel = (i == 0)
    if sel: s.fill(8, y, 224, 40, C_CARD)
    bg = C_CARD if sel else C_BG
    if i == 0:
        s.text(16, y + 3, "自动息屏", C_DIM, bg)
        s.rtext(216, y + 3, "10 分钟", C_ACCENT, bg)
        s.rtext(216, y + 21, "›", C_DIM, bg)
    else:
        s.ctext(120, y + 11, ("立即息屏" if i == 1 else "‹ 返回总览"),
                (C_OK if i == 1 else C_ACCENT) if sel else C_TEXT, bg)
s.text(10, 216, "息屏后 WiFi 仍在线", C_DIM, C_BG)
s.text(10, 234, "按任意键唤醒屏幕", C_DIM, C_BG)
footer(s, "上下选择  OK 确认")
s.img.save(f"{OUTDIR}/page_settings.png")

# ---------- 6. 自动息屏时间 ----------
s = Screen()
header(s, "自动息屏")
opts = ["10 分钟", "20 分钟", "30 分钟", "60 分钟", "自定义 15 分钟", "‹ 返回设置"]
for i, lb in enumerate(opts):
    y = 34 + i * 34
    sel = (i == 4)
    if sel: s.fill(6, y, 228, 30, C_CARD)
    bg = C_CARD if sel else C_BG
    editing = (sel and i == 4)
    col = C_WARN if editing else C_TEXT
    s.text(16, y + 7, lb, col, bg)
    if i < 4:
        s.rtext(220, y + 7, "当前" if i == 0 else "›", C_OK if i == 0 else C_DIM, bg)
    elif i == 4:
        s.rtext(220, y + 7, "OK保存", C_WARN, bg)
footer(s, "上下调数值 OK保存")
s.img.save(f"{OUTDIR}/page_sleepopt.png")
print("saved settings pages")

# ---------- 7. 首页（2x2 四卡 + 最近动态 + 三个入口）----------
s = Screen()
header(s, "拓竹农场")
cw, cardH, gap, x0, y0 = 112, 72, 8, 8, 34
cards = [("打印中", 3, C_OK, "最快8分"), ("空闲", 30, C_DIM, "可下发"),
         ("异常", 2, C_WARN, "停1 障1"), ("队列", 7, C_ACCENT, "7个任务")]
focus = 0
for i, (lab, val, col, sub) in enumerate(cards):
    x = x0 + (i & 1) * (cw + gap)
    y = y0 + (i >> 1) * (cardH + gap)
    foc = (i == focus)
    bg = C_CARD2 if foc else C_CARD
    s.fill(x, y, cw, cardH, bg)
    if foc:
        s.fill(x, y, cw, 1, C_ACCENT); s.fill(x, y + cardH - 1, cw, 1, C_ACCENT)
        s.fill(x, y, 1, cardH, C_ACCENT); s.fill(x + cw - 1, y, 1, cardH, C_ACCENT)
    s.text(x + 10, y + 5, lab, C_TEXT if foc else C_DIM, bg)
    s.big(x + cw // 2, y + 20, str(val), col, bg, k=2)
    s.text(x + 10, y + 52, sub, C_DIM, bg)

ly = 192
s.text(8, ly, "最近动态", C_DIM, C_BG)
ly += 20
s.text(8, ly, "A1-01 打印完成", C_OK, C_BG)
ly += 18
s.text(8, ly, "A1-c1 暂停 热床温度异常", C_WARN, C_BG)

tabs2 = ["事件", "设置", "全部设备"]
for i, t in enumerate(tabs2):
    x = i * 80
    act = (focus == 4 + i)
    s.fill(x, 272, 80, 22, C_CARD if act else C_BG)
    s.ctext(x + 40, 275, t, C_ACCENT if act else C_DIM, C_CARD if act else C_BG)
footer(s, "上下选择  OK 进入")
s.img.save(f"{OUTDIR}/page_home_new.png")

# ---------- 8. 二级列表（分类 + 翻页 + 滚动条）----------
s = Screen()
header(s, "打印中 3台")
names = ["A1-01", "A1-03", "A1-c3", "A1-b2", "A1-d1", "A1-d2", "A1-d3", "A1-e1", "A1-e2"]
progs = [69, 94, 63, 12, 45, 88, 30, 55, 71]
rems = [42, 8, 55, 90, 33, 15, 60, 25, 48]
total = len(names) + 1
vis, rowH = 6, 38
sel = 3
scroll = (sel >= vis) and sel - vis + 1 or 0
for r in range(vis):
    idx = r + scroll
    if idx >= total: break
    y = 30 + r * rowH
    isSel = (idx == sel)
    bg = C_CARD if isSel else C_BG
    if isSel: s.fill(4, y, 232, rowH - 2, bg)
    if idx == 0:
        s.text(12, y + 9, "‹ 返回总览", C_ACCENT, bg); continue
    n = names[idx - 1]
    s.text(10, y + 2, n, C_TEXT, bg)
    s.text(10, y + 20, "打印中", C_OK, bg)
    s.bar(96, y + 22, 66, 7, progs[idx - 1], C_OK)
    s.rtext(228, y + 2, f"{progs[idx-1]}%", C_TEXT, bg)
    s.rtext(228, y + 20, f"{rems[idx-1]}分", C_DIM, bg)
if total > vis:
    sh = max(14, 228 * vis // total)
    sy = 30 + (228 - sh) * scroll // (total - vis)
    s.fill(234, sy, 4, sh, C_DIM)
footer(s, "上下翻页  OK 详情")
s.img.save(f"{OUTDIR}/page_catlist.png")
print("saved home+catlist")

# ---------- 9. 打印设置页 ----------
s = Screen()
header(s, "打印设置")
items = [("自动调平", True), ("流量校准", True), ("延时摄影", False)]
y0, rowH = 42, 40
for i, (lab, on) in enumerate(items):
    y = y0 + i * rowH
    sel = (i == 0)
    if sel: s.fill(8, y, 224, rowH - 6, C_CARD)
    bg = C_CARD if sel else C_BG
    s.text(18, y + 8, lab, C_TEXT if sel else C_DIM, bg)
    col = C_OK if on else C_DIM
    # 开关框
    s.fill(150, y + 5, 62, 1, col); s.fill(150, y + 28, 62, 1, col)
    s.fill(150, y + 5, 1, 24, col); s.fill(211, y + 5, 1, 24, col)
    s.ctext(181, y + 9, "开" if on else "关", col, bg)
sel = False
s.ctext(120, y0 + 3 * rowH + 9, "‹ 返回确认页", C_ACCENT, C_BG)
s.text(10, 232, "默认：调平开 校准开 延时关", C_DIM, C_BG)
s.text(10, 250, "OK 切换开关", C_DIM, C_BG)
footer(s, "上下选择  OK 切换")
s.img.save(f"{OUTDIR}/page_printopt.png")

# ---------- 10. 确认下发（四行）----------
s = Screen()
header(s, "确认下发")
s.text(10, 34, "1_plate_5", C_TEXT, C_BG)
s.text(10, 54, "剩15件  本次派2台", C_DIM, C_BG)
y0, rowH = 72, 36
rows = [("料盘映射", "外挂料盘", "›", True),
        ("已选机台", "A1-02 A1-04", "", False),
        ("打印设置", "调平开 校准开 延时关", "›", False)]
for i, (t1, t2, r, sel) in enumerate(rows):
    y = y0 + i * rowH
    bg = C_CARD if sel else C_BG
    if sel: s.fill(6, y, 228, rowH - 4, bg)
    s.text(14, y + 2, t1, C_DIM, bg)
    s.text(14, y + 18, t2, C_ACCENT if i == 0 else C_TEXT, bg)
    if r: s.rtext(226, y + 10, r, C_ACCENT if sel else C_DIM, bg)
y = y0 + 3 * rowH
s.fill(6, y, 228, 34, C_CARD)
s.ctext(120, y + 9, "确认下发", C_OK, C_CARD)
s.text(10, 226, "清盘由电脑端建任务时确定", C_DIM, C_BG)
footer(s, "上下选择  OK 执行")
s.img.save(f"{OUTDIR}/page_options2.png")
print("saved printopt+options")
