#!/usr/bin/env python3
"""Генерирует include/linux/font16.h: растровый шрифт 8x16 из GNU Unifont (unifont.otf, GPL-2+ со
исключением для встраивания шрифтов). Набор знаков: ASCII, Latin-1, кириллица, знаки рамок и блоков,
типографские знаки. Запуск: tools/mkfont.py /usr/share/fonts/opentype/unifont/unifont.otf > include/linux/font16.h"""
import sys
from PIL import Image, ImageDraw, ImageFont

RANGES = [
    (0x00, 0x7F),      # ASCII; 0..31 и 127 -- пустые ячейки (индекс = код)
    (0xA0, 0xFF),      # Latin-1
    (0x400, 0x45F),    # кириллица (рус., укр., белорус., сербск., македонск.)
    (0x490, 0x491),    # Ґ ґ
    (0x2010, 0x2015), (0x2018, 0x201E), (0x2020, 0x2022), (0x2026, 0x2026), (0x2030, 0x2030),
    (0x20AC, 0x20AC), (0x20BD, 0x20BD), (0x2116, 0x2116), (0x2122, 0x2122),
    (0x2190, 0x2195), (0x21B5, 0x21B5),
    (0x2212, 0x2212), (0x221A, 0x221A), (0x221E, 0x221E), (0x2248, 0x2248), (0x2260, 0x2261), (0x2264, 0x2265),
    (0x2500, 0x257F),  # рамки
    (0x2580, 0x259F),  # блоки и тени
    (0x25A0, 0x25A1), (0x25AA, 0x25AB), (0x25B2, 0x25B2), (0x25BA, 0x25BA), (0x25BC, 0x25BC), (0x25C4, 0x25C4),
    (0x25CB, 0x25CB), (0x25CF, 0x25CF), (0x2713, 0x2714), (0x2022, 0x2022),
]

font = ImageFont.truetype(sys.argv[1], 16)
def glyph(cp):
    if cp < 0x20 or cp == 0x7F:
        return [0] * 16
    img = Image.new("1", (16, 16), 0)
    ImageDraw.Draw(img).text((0, 0), chr(cp), font=font, fill=1)
    px = img.load()
    # ширина полуширинного знака Unifont -- 8 пикселей; правая половина должна быть пустой
    if any(px[x, y] for x in range(8, 16) for y in range(16)):
        return None
    return [sum((1 << (7 - x)) for x in range(8) if px[x, y]) for y in range(16)]

glyphs, ranges = [], []
seen = set()
for lo, hi in RANGES:
    base = len(glyphs)
    run_lo = None
    for cp in range(lo, hi + 1):
        if cp in seen:
            continue
        g = glyph(cp)
        if g is None:                       # двойной ширины или нет в шрифте -- пропустить
            if run_lo is not None:
                ranges.append((run_lo, cp - 1, run_base)); run_lo = None
            continue
        if run_lo is None:
            run_lo, run_base = cp, len(glyphs)
        seen.add(cp)
        glyphs.append(g)
    if run_lo is not None:
        ranges.append((run_lo, hi, run_base))
# запасной знак '?' -- индекс 0x3F (ASCII идёт первым, индекс = код)
out = ["/* Сгенерировано tools/mkfont.py из GNU Unifont (GPL-2+, исключение для шрифтов). Не править руками. */",
       "#ifndef _LINUX_FONT16_H", "#define _LINUX_FONT16_H", "",
       "#include <linux/types.h>", "",
       "#define FONT_W 8", "#define FONT_H 16", "#define FONT_GLYPHS %d" % len(glyphs), "#define FONT_FALLBACK 0x3F", "",
       "struct font_range { uint16_t first, last, base; };",
       "static const struct font_range font_ranges[] = {"]
out += ["    { 0x%04X, 0x%04X, %d }," % r for r in ranges]
out += ["};", "#define FONT_NRANGES %d" % len(ranges), "", "static const uint8_t font16[FONT_GLYPHS][16] = {"]
for i, g in enumerate(glyphs):
    out.append("    {" + ",".join("0x%02X" % b for b in g) + "},")
out += ["};", "", "#endif"]
print("\n".join(out))
