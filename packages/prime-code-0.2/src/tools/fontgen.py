#!/usr/bin/env python3
"""Generate a compact C source from a GNU Unifont .hex file.

The generated subset contains U+0020..U+007E. Each glyph is represented as
16 rows of uint16_t; 8x16 glyphs use the low 8 bits and have width 8,
16x16 glyphs use all 16 bits and have width 16.
"""
from pathlib import Path
import sys

START, END = 0x20, 0x7E


def parse_hex(path: Path):
    glyphs = {}
    with path.open("r", encoding="ascii", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            left, hexdata = line.split(":", 1)
            try:
                cp = int(left, 16)
            except ValueError:
                continue
            if START <= cp <= END:
                try:
                    raw = bytes.fromhex(hexdata)
                except ValueError:
                    continue
                if len(raw) not in (16, 32):
                    continue
                width = 8 if len(raw) == 16 else 16
                rows = []
                if width == 8:
                    rows = list(raw)
                else:
                    for i in range(0, 32, 2):
                        rows.append((raw[i] << 8) | raw[i + 1])
                if len(rows) == 16:
                    glyphs[cp] = (width, rows)
    return glyphs


def c_hex16(v):
    return f"0x{v:04X}"


def generate(hex_path: Path, out_c: Path, out_h: Path):
    glyphs = parse_hex(hex_path)
    missing = [cp for cp in range(START, END + 1) if cp not in glyphs]
    if missing:
        raise SystemExit("Missing Unifont glyphs: " + ", ".join(f"U+{cp:04X}" for cp in missing))

    rows = []
    widths = []
    for cp in range(START, END + 1):
        w, r = glyphs[cp]
        widths.append(w)
        rows.extend(r)

    out_h.write_text(
        "#ifndef UNIFONT_FONT_H\n"
        "#define UNIFONT_FONT_H\n"
        "#include <stdint.h>\n"
        "extern const uint16_t unifont_rows[95][16];\n"
        "extern const uint8_t unifont_widths[95];\n"
        "const uint16_t *unifont_glyph(unsigned char c, uint8_t *width);\n"
        "#endif\n",
        encoding="ascii",
    )

    with out_c.open("w", encoding="ascii") as f:
        f.write('#include "unifont_font.h"\n\n')
        f.write("/* Generated from GNU Unifont 17.0.04, ASCII U+0020..U+007E. */\n")
        f.write("const uint16_t unifont_rows[95][16] = {\n")
        for idx, cp in enumerate(range(START, END + 1)):
            w, r = glyphs[cp]
            f.write("    {\n        ")
            f.write(", ".join(c_hex16(v) for v in r))
            f.write("\n    }")
            f.write("," if idx < 94 else "")
            f.write(f" /* U+{cp:04X} {chr(cp)!r}, {w}x16 */\n")
        f.write("};\n\n")
        f.write("const uint8_t unifont_widths[95] = {\n    ")
        for i, w in enumerate(widths):
            if i and i % 16 == 0:
                f.write("\n    ")
            f.write(str(w))
            f.write(", " if i < 94 else "")
        f.write("\n};\n\n")
        f.write(
            "const uint16_t *unifont_glyph(unsigned char c, uint8_t *width) {\n"
            "    if (c < 0x20 || c > 0x7E) c = '?';\n"
            "    c = (unsigned char)(c - 0x20);\n"
            "    if (width) *width = unifont_widths[c];\n"
            "    return unifont_rows[c];\n"
            "}\n"
        )


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} INPUT.hex OUTPUT.c OUTPUT.h")
        raise SystemExit(2)
    generate(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
