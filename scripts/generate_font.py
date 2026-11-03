#!/usr/bin/env python3
"""
Generate an LVGL v9 compatible C font for CJK, Korean Hangul, and extended symbols.
Extracts glyphs from NotoSansCJK-VF.ttc.
"""

import sys
import time
from PIL import ImageFont

def build_codepoint_list():
    # 1. Common symbols, punctuation, quotes, dashes, stars, musical notes
    punct = [
        0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x2013, 0x2014, 0x2022, 0x2027,
        0x25A0, 0x25A1, 0x25CB, 0x25CF, 0x25CE, 0x25C6, 0x25C7,
        0x2605, 0x2606, 0x2661, 0x2665, 0x2669, 0x266A, 0x266B, 0x266C,
        0x203B, 0x2103, 0x2713
    ]

    # 2. CJK Symbols & Punctuation (0x3000 - 0x303F)
    cjk_symbols = list(range(0x3000, 0x3040))

    # 3. Hiragana (0x3041 - 0x3096)
    hiragana = list(range(0x3041, 0x3097))

    # 4. Katakana (0x30A1 - 0x30FA) + prolonged mark (0x30FC)
    katakana = list(range(0x30A1, 0x30FB)) + [0x30FC]

    # 5. Hangul Compatibility Jamo (0x3131 - 0x318E)
    hangul_jamo = list(range(0x3131, 0x318F))

    # 6. JIS Level 1 Kanji (2,965 Kanji)
    jis_kanji = []
    for b1 in range(0xb0, 0xd0):
        for b2 in range(0xa1, 0xff):
            try:
                ch = bytes([b1, b2]).decode('euc-jp')
                if ord(ch) >= 0x4e00:
                    jis_kanji.append(ord(ch))
            except Exception:
                pass

    # 7. KS X 1001 Hangul Syllables (2,350 Syllables)
    ks_hangul = []
    for b1 in range(0xb0, 0xc9):
        for b2 in range(0xa1, 0xff):
            try:
                ch = bytes([b1, b2]).decode('euc-kr')
                ks_hangul.append(ord(ch))
            except Exception:
                pass

    group1 = sorted(set(punct))
    group2 = sorted(set(cjk_symbols + hiragana + katakana + hangul_jamo))
    group3 = sorted(set(jis_kanji))
    group4 = sorted(set(ks_hangul))

    return [
        ("symbols", group1),
        ("kana_jamo", group2),
        ("kanji", group3),
        ("hangul", group4)
    ]

def generate_font_c(font_path, font_size, output_path):
    t0 = time.time()
    font = ImageFont.truetype(font_path, font_size)
    ascent, descent = font.getmetrics()
    baseline = ascent
    line_height = ascent + descent

    groups = build_codepoint_list()

    glyph_bitmaps = bytearray()
    glyph_dscs = [] # (bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y)
    cmap_entries = [] # (range_start, range_length, glyph_id_start, unicode_list)

    # Glyph ID 0 is reserved
    glyph_dscs.append((0, 0, 0, 0, 0, 0))
    current_glyph_id = 1

    for name, cps in groups:
        if not cps:
            continue
        range_start = min(cps)
        range_length = max(cps) - range_start + 1
        unicode_list = [cp - range_start for cp in cps]
        cmap_entries.append((range_start, range_length, current_glyph_id, unicode_list, name))

        for cp in cps:
            ch = chr(cp)
            mask = font.getmask(ch, mode='1')
            w, h = mask.size
            adv_w_flt = font.getlength(ch)
            adv_w = int(round(adv_w_flt * 16))

            if w == 0 or h == 0:
                glyph_dscs.append((len(glyph_bitmaps), adv_w, 0, 0, 0, 0))
            else:
                bbox = font.getbbox(ch)
                ofs_x = bbox[0]
                ofs_y = baseline - bbox[3]

                bitmap_index = len(glyph_bitmaps)

                # Pack 1bpp MSB first
                bits = []
                for y in range(h):
                    for x in range(w):
                        bits.append(1 if mask.getpixel((x, y)) else 0)

                cur_byte = 0
                bit_cnt = 0
                for b in bits:
                    cur_byte = (cur_byte << 1) | b
                    bit_cnt += 1
                    if bit_cnt == 8:
                        glyph_bitmaps.append(cur_byte)
                        cur_byte = 0
                        bit_cnt = 0
                if bit_cnt > 0:
                    cur_byte <<= (8 - bit_cnt)
                    glyph_bitmaps.append(cur_byte)

                glyph_dscs.append((bitmap_index, adv_w, w, h, ofs_x, ofs_y))

            current_glyph_id += 1

    print(f"Generated {len(glyph_dscs)-1} glyphs in {time.time()-t0:.2f}s")
    print(f"Bitmap data: {len(glyph_bitmaps)} bytes ({len(glyph_bitmaps)/1024:.1f} KB)")
    print(f"Descriptor data: {len(glyph_dscs)*8} bytes ({len(glyph_dscs)*8/1024:.1f} KB)")

    with open(output_path, 'w', encoding='utf-8') as out:
        out.write("/* Auto-generated LVGL font for CJK, Korean Hangul, and extended symbols */\n")
        out.write("#include \"lvgl.h\"\n\n")

        # Bitmaps
        out.write(f"static const uint8_t glyph_bitmap[{len(glyph_bitmaps)}] = {{\n")
        # Format 16 bytes per line
        for i in range(0, len(glyph_bitmaps), 16):
            chunk = glyph_bitmaps[i:i+16]
            out.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        out.write("};\n\n")

        # Descriptors
        out.write(f"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[{len(glyph_dscs)}] = {{\n")
        out.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
        for d in glyph_dscs[1:]:
            out.write(f"    {{.bitmap_index = {d[0]}, .adv_w = {d[1]}, .box_w = {d[2]}, .box_h = {d[3]}, .ofs_x = {d[4]}, .ofs_y = {d[5]}}},\n")
        out.write("};\n\n")

        # CMAP unicode lists
        for _, _, _, ulist, name in cmap_entries:
            out.write(f"static const uint16_t unicode_list_{name}[{len(ulist)}] = {{\n")
            for i in range(0, len(ulist), 12):
                chunk = ulist[i:i+12]
                out.write("    " + ", ".join(f"0x{u:04x}" for u in chunk) + ",\n")
            out.write("};\n\n")

        # CMAPs table
        out.write(f"static const lv_font_fmt_txt_cmap_t cmaps[{len(cmap_entries)}] = {{\n")
        for r_start, r_len, gid_start, ulist, name in cmap_entries:
            out.write("    {\n")
            out.write(f"        .range_start = 0x{r_start:04X},\n")
            out.write(f"        .range_length = {r_len},\n")
            out.write(f"        .glyph_id_start = {gid_start},\n")
            out.write(f"        .unicode_list = unicode_list_{name},\n")
            out.write("        .glyph_id_ofs_list = NULL,\n")
            out.write(f"        .list_length = {len(ulist)},\n")
            out.write("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
            out.write("    },\n")
        out.write("};\n\n")

        # Font dsc and lv_font_t
        out.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        out.write("    .glyph_bitmap = glyph_bitmap,\n")
        out.write("    .glyph_dsc = glyph_dsc,\n")
        out.write("    .cmaps = cmaps,\n")
        out.write("    .kern_dsc = NULL,\n")
        out.write("    .kern_scale = 0,\n")
        out.write(f"    .cmap_num = {len(cmap_entries)},\n")
        out.write("    .bpp = 1,\n")
        out.write("    .kern_classes = 0,\n")
        out.write("    .bitmap_format = 0,\n")
        out.write("    .stride = 0,\n")
        out.write("};\n\n")

        out.write("const lv_font_t font_cjk_kr_14 = {\n")
        out.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        out.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        out.write(f"    .line_height = {line_height},\n")
        out.write(f"    .base_line = {descent},\n")
        out.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
        out.write("    .underline_position = -2,\n")
        out.write("    .underline_thickness = 1,\n")
        out.write("    .dsc = &font_dsc,\n")
        out.write("    .fallback = NULL,\n")
        out.write("    .user_data = NULL,\n")
        out.write("};\n")

    print(f"Written font to {output_path} successfully!")

if __name__ == '__main__':
    generate_font_c(
        '/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc',
        14,
        'main/font_cjk_kr_14.c'
    )
