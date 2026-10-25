#!/usr/bin/env python3
"""为 app-collection 的应用生成**纯主图**图标（38×38，无圆角、无角标）。

⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

只做一件事：把各应用的功能图形画在纯色底上，输出满幅方形 PNG。
**不裁圆角、不加角标** —— 圆角与角标由使用者手动处理
（例如用 `icon 工具.html`：上传本脚本产出的 icon.png，再设圆角/角标后导出）。

绘制在 4× 超采样画布上完成后缩小，边缘平滑。

用法：
    python3 tools/make-icons.py            # 生成全部
    python3 tools/make-icons.py --dry-run  # 只列出目标，不写文件

依赖：仅 Pillow（pip install Pillow）。
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

SIZE = 38                 # 输出边长
SS = 4                    # 超采样倍数

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # app-collection/


# ---------------------------------------------------------------- 主题图形 --
# 每个函数在 S×S 的超采样画布上作画，(0,0) 为左上角；fg 为图形颜色。

def _shade(hexcolor, k):
    """按比例压暗（0..1）。"""
    h = hexcolor.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return (int(r * k), int(g * k), int(b * k))


def draw_fruit(d, S, fg):
    """水果：圆 + 顶部叶。"""
    r = S * 0.26
    cx, cy = S * 0.5, S * 0.58
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fg)
    d.polygon([(cx, cy - r * 1.02), (cx + S * 0.16, cy - r * 1.5),
               (cx + S * 0.06, cy - r * 1.05)], fill=fg)


def draw_cube(d, S, fg):
    """等距立方体：顶面 + 左右面。"""
    cx, cy, a = S * 0.5, S * 0.5, S * 0.27
    d.polygon([(cx, cy - a), (cx + a, cy - a * 0.5), (cx, cy), (cx - a, cy - a * 0.5)], fill=fg)
    d.polygon([(cx - a, cy - a * 0.5), (cx, cy), (cx, cy + a), (cx - a, cy + a * 0.5)], fill=_shade(fg, 0.72))
    d.polygon([(cx + a, cy - a * 0.5), (cx + a, cy + a * 0.5), (cx, cy + a), (cx, cy)], fill=_shade(fg, 0.5))


def draw_keys(d, S, fg):
    """键盘：3×2 键格，其中一键高亮。"""
    m, g = S * 0.24, S * 0.09
    bw, bh = (S - 2 * m - 2 * g) / 3, (S - 2 * m - g) / 2
    for r in range(2):
        for c in range(3):
            x = m + c * (bw + g)
            y = m + r * (bh + g)
            col = fg if (r == 1 and c == 1) else _shade(fg, 0.45)
            d.rounded_rectangle([x, y, x + bw, y + bh], radius=S * 0.03, fill=col)


def draw_brush(d, S, fg):
    """画笔：斜笔杆 + 笔尖，加两个色点。"""
    d.polygon([(S * 0.30, S * 0.62), (S * 0.62, S * 0.24),
               (S * 0.74, S * 0.36), (S * 0.42, S * 0.74)], fill=fg)
    d.polygon([(S * 0.28, S * 0.64), (S * 0.40, S * 0.76), (S * 0.24, S * 0.80)], fill=fg)
    d.ellipse([S * 0.20, S * 0.22, S * 0.30, S * 0.32], fill=_shade(fg, 0.6))
    d.ellipse([S * 0.72, S * 0.68, S * 0.82, S * 0.78], fill=_shade(fg, 0.6))


def draw_curve(d, S, fg):
    """坐标轴 + 正弦曲线。"""
    ax = S * 0.24
    w = max(1, int(S * 0.02))
    d.line([(ax, S * 0.18), (ax, S * 0.80)], fill=_shade(fg, 0.6), width=w)
    d.line([(ax * 0.6, S * 0.76), (S * 0.84, S * 0.76)], fill=_shade(fg, 0.6), width=w)
    pts = []
    n = 40
    for i in range(n + 1):
        t = i / n
        pts.append((ax + t * (S * 0.84 - ax),
                    S * 0.48 - math.sin(t * math.pi * 2) * S * 0.24))
    d.line(pts, fill=fg, width=max(2, int(S * 0.045)), joint="curve")


def draw_code(d, S, fg):
    """代码：左右尖括号 + 中间斜杠。"""
    w = max(2, int(S * 0.05))
    d.line([(S * 0.36, S * 0.30), (S * 0.20, S * 0.50), (S * 0.36, S * 0.70)], fill=fg, width=w, joint="curve")
    d.line([(S * 0.64, S * 0.30), (S * 0.80, S * 0.50), (S * 0.64, S * 0.70)], fill=fg, width=w, joint="curve")
    d.line([(S * 0.56, S * 0.26), (S * 0.44, S * 0.74)], fill=_shade(fg, 0.7), width=w)


def draw_doc(d, S, fg):
    """文档：纸张 + 折角 + 文本行。"""
    x0, y0, x1, y1 = S * 0.26, S * 0.18, S * 0.74, S * 0.82
    fold = S * 0.15
    d.polygon([(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)], fill=fg)
    d.polygon([(x1 - fold, y0), (x1, y0 + fold), (x1 - fold, y0 + fold)], fill=_shade(fg, 0.5))
    for i in range(3):
        yy = y0 + fold + S * 0.10 + i * S * 0.11
        d.line([(x0 + S * 0.07, yy), (x1 - S * 0.07, yy)], fill=_shade(fg, 0.45),
               width=max(1, int(S * 0.025)))


def draw_play(d, S, fg):
    """播放三角（= 运行 ELF）。"""
    d.polygon([(S * 0.34, S * 0.22), (S * 0.34, S * 0.78), (S * 0.78, S * 0.50)], fill=fg)


# ---------------------------------------------------------------- 清单 ------

APPS = [
    # (相对路径, 主色, 主题绘制函数)
    ("examples/suika/suika.hpappdir/icon.png",                  "#15803d", draw_fruit),
    ("examples/cube3d/cube3d.hpappdir/icon.png",                "#2f6fd0", draw_cube),
    ("examples/keymon/keymon.hpappdir/icon.png",                "#4a5568", draw_keys),
    ("examples/paint/paint.hpappdir/icon.png",                  "#7c4fd9", draw_brush),
    ("examples/graph/graph.hpappdir/icon.png",                  "#2fa36b", draw_curve),
    ("staging/prime-code/primecode.hpappdir/icon.png",          "#334155", draw_code),
    ("staging/prime-markdown/prime-markdown.hpappdir/icon.png", "#0f766e", draw_doc),
    ("tools/runelf/runelf.hpappdir/icon.png",                   "#475569", draw_play),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    for rel, bg, painter in APPS:
        if args.dry_run:
            print(f"  [dry-run] {rel}  bg={bg}")
            continue

        S = SIZE * SS
        px = Image.new("RGBA", (1, 1), bg).getpixel((0, 0))
        img = Image.new("RGBA", (S, S), px)          # 满幅纯色底
        painter(ImageDraw.Draw(img), S, "#ffffff")   # 图形

        icon = img.resize((SIZE, SIZE), Image.LANCZOS)
        os.makedirs(os.path.dirname(os.path.join(ROOT, rel)), exist_ok=True)
        icon.save(os.path.join(ROOT, rel), "PNG")
        print(f"  {rel}  ({icon.size[0]}x{icon.size[1]}, 无圆角/无角标)")


if __name__ == "__main__":
    sys.exit(main())
