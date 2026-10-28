#!/usr/bin/env python3
"""
genfont.py — 从 Windows TTF 渲染中文点阵字库，输出 cnfont.h

规则：
  - 从 .ino 源码里自动扫描所有用到的字符（UTF-8）
  - 中文按 16x16 / 12x12 渲染（1bit，行优先，宽度按字节对齐）
  - ASCII 用半宽（8px / 6px）
  - 输出码点索引表（按码点排序，二分查找）
"""
import re
import sys
import os
import json
from PIL import Image, ImageDraw, ImageFont

INO = r"C:\Users\zzz\WorkBuddy\农场\firmware\aipassport_farm\aipassport_farm.ino"
OUT = r"C:\Users\zzz\WorkBuddy\农场\firmware\aipassport_farm\cnfont.h"

# 是否把 GB2312 全字库（6763 个汉字）也编进去。
# 界面上很多中文是运行时从服务器来的（故障原因、设备名），源码扫不到，
# 必须整体收进来才不会显示成方框。加 --no-gb 可以只按源码生成（用于调试体积）。
ADD_GB2312 = '--no-gb' not in __import__('sys').argv

# 字体候选（Windows 自带）
FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\msyh.ttc", 0),      # 微软雅黑
    (r"C:\Windows\Fonts\msyh.ttf", 0),
    (r"C:\Windows\Fonts\simhei.ttf", 0),    # 黑体
    (r"C:\Windows\Fonts\Deng.ttf", 0),      # 等线
    (r"C:\Windows\Fonts\simsun.ttc", 0),    # 宋体
]

def find_font(size):
    for path, idx in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size, index=idx)
            except Exception:
                continue
    raise SystemExit("找不到可用中文字体")

def gb2312_chars(level1=True, level2=True):
    """GB2312 全部汉字：一级 3755 个（常用）+ 二级 3008 个（次常用）。
    必须把它们也收进字库：界面上很多文字是运行时从服务器拿的
    （设备名、HMS 故障原因等），脚本扫描 .ino 源码扫不到，
    少了就是满屏方框。"""
    out = []
    ranges = []
    if level1: ranges.append((0xB0, 0xD7))   # 一级字库
    if level2: ranges.append((0xD8, 0xF7))   # 二级字库
    for a, b in ranges:
        for hi in range(a, b + 1):
            for lo in range(0xA1, 0xFF):
                try:
                    out.append(bytes([hi, lo]).decode('gb2312'))
                except Exception:
                    pass
    return out


def collect_chars():
    """扫描 .ino 里所有字符串字面量中的字符"""
    src = open(INO, encoding='utf-8').read()
    chars = set()
    # 所有非 ASCII 字符 + 数字/字母/常用符号
    for ch in src:
        if ord(ch) > 127:
            chars.add(ch)
    # ASCII 可打印
    for c in range(0x20, 0x7F):
        chars.add(chr(c))
    # 换行/制表不算
    chars.discard('\r')
    # ★ 运行时动态文本（服务器下发）用的汉字，源码里没有，必须整体收进来
    if ADD_GB2312:
        chars.update(gb2312_chars())
    chars.discard('\n')
    chars.discard('\t')
    return chars

def render(chars, px, ascii_w, out_prefix, threshold=110):
    """渲染字库，返回 (C 代码, 码点列表)"""
    SS = 2                      # 超采样倍数
    font = find_font(px * SS)   # 在 2 倍画布上用 2 倍字号，缩回来才是 px
    glyphs = []   # (cp, w, h, bytes)
    for ch in sorted(chars, key=ord):
        cp = ord(ch)
        is_ascii = cp < 128
        w = ascii_w if is_ascii else px
        h = px
        canvas = px * SS
        img = Image.new('L', (canvas, canvas), 0)
        d = ImageDraw.Draw(img)
        try:
            bbox = d.textbbox((0, 0), ch, font=font)
            tw = bbox[2] - bbox[0]
            th = bbox[3] - bbox[1]
            d.text((canvas / 2 - tw / 2 - bbox[0], canvas / 2 - th / 2 - bbox[1]), ch, fill=255, font=font)
        except Exception:
            pass
        img = img.resize((px, px), Image.LANCZOS)
        if is_ascii:
            # ASCII：按实际墨迹宽度裁（比例字体），最宽 12px
            pd = img.load()
            minx, maxx = px, -1
            for y in range(h):
                for x in range(px):
                    if pd[x, y] > threshold:
                        minx = min(minx, x); maxx = max(maxx, x)
            if maxx < minx:
                minx, maxx = 0, ascii_w - 1
            span = maxx - minx + 1
            if span > 12:
                span = 12
            w = max(3, span)
            crop = Image.new('L', (w, h), 0)
            crop.paste(img.crop((minx, 0, minx + w, h)), (0, 0))
        else:
            crop = img.crop((0, 0, w, h))
        # 打包 1bit
        data = bytearray()
        pd = crop.load()
        for y in range(h):
            acc = 0
            bits = 0
            for x in range(w):
                acc = (acc << 1) | (1 if pd[x, y] > threshold else 0)
                bits += 1
                if bits == 8:
                    data.append(acc); acc = 0; bits = 0
            if bits:
                data.append(acc << (8 - bits))
        glyphs.append((cp, w, h, bytes(data)))

    total = sum(4 + 4 + 2 + 2 + len(g[3]) for g in glyphs)
    # 生成 C 代码
    lines = []
    lines.append(f"// 自动生成：{out_prefix}  共 {len(glyphs)} 字形，{total} 字节")
    lines.append(f"")
    lines.append(f"#define {out_prefix}_SIZE {px}")
    lines.append(f"#define {out_prefix}_COUNT {len(glyphs)}")
    lines.append(f"")
    # 位图
    lines.append(f"static const uint8_t {out_prefix}_BITS[] = {{")
    off = 0
    for cp, w, h, data in glyphs:
        lines.append("  " + ",".join(str(b) for b in data) + ",")
    lines.append(f"}};")
    lines.append(f"")
    # 索引
    lines.append(f"static const CnGlyph {out_prefix}_MAP[] = {{")
    off = 0
    for cp, w, h, data in glyphs:
        lines.append(f"  {{{off},{cp},{w},{h}}},")   # 顺序必须和 CnGlyph 定义一致
        off += len(data)
    lines.append(f"}};")
    return "\n".join(lines), glyphs

def main():
    chars = collect_chars()
    cjk = [c for c in chars if ord(c) > 127]
    print(f"总字符 {len(chars)}（其中非 ASCII {len(cjk)}）")
    print("非 ASCII 字符:", "".join(sorted(cjk, key=ord))[:200])

    c16, g16 = render(chars, 16, 8, "CN16")
    c12, g12 = render(chars, 12, 6, "CN12")
    # 两个字号共用一个 CnGlyph 结构定义，去掉重复的
    c12 = c12.replace("#pragma once\n#include <stdint.h>\n", "")
    c12 = re.sub(r"typedef struct \{.*?\} CnGlyph;\n", "", c12, flags=re.S)

    header = f"""// ===== 自动生成的中文点阵字库（勿手改）=====
// 生成脚本：tools/genfont.py
// 字形数：16px {len(g16)} 个 / 12px {len(g12)} 个
#pragma once
#include <stdint.h>

// off 必须是 32 位：收进 GB2312 全字库后位图超过 65535 字节，
// uint16_t 会溢出，字就全画错了。
typedef struct {{
  uint32_t off; uint16_t cp; uint8_t w; uint8_t h;
}} CnGlyph;

{c16}

{c12}
"""
    open(OUT, 'w', encoding='utf-8').write(header)
    size = os.path.getsize(OUT)
    print(f"输出 {OUT}  ({size} 字节)")
    print(f"16px: {len(g16)} 字形 / 12px: {len(g12)} 字形")

if __name__ == '__main__':
    main()
